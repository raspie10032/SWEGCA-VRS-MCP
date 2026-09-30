#include "world/byte_retriever.hpp"

#include "swegca_architecture/sha256.hpp"
#include "transport/json.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory_resource>
#include <numbers>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace swegca::world {
namespace {

using Vec = std::vector<float>;
constexpr std::string_view checkpoint_schema = "mosaic-byte-retriever-checkpoint-v0";
constexpr std::string_view manifest_schema = "mosaic-byte-retriever-manifest-v0";
constexpr double layer_norm_epsilon = 1e-5;

[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }

double rounded_six(const double value) {
    return std::nearbyint(value * 1'000'000.0) / 1'000'000.0;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open byte retriever input: " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::vector<std::string_view> lines(const std::string_view text) {
    std::vector<std::string_view> result;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        result.push_back(line);
        begin = end == text.size() ? text.size() : end + 1;
    }
    if (!result.empty() && result.back().empty() && !text.empty() && text.back() == '\n')
        result.pop_back();
    return result;
}

std::array<std::string_view, 4> four_columns(const std::string_view line) {
    std::array<std::string_view, 4> result;
    std::size_t begin = 0;
    for (std::size_t index = 0; index != result.size(); ++index) {
        const auto end = index + 1 == result.size() ? line.size() : line.find('\t', begin);
        if (end == std::string_view::npos) invalid("qrels row must contain four tab-separated fields");
        result[index] = line.substr(begin, end - begin);
        begin = end + 1;
    }
    return result;
}

std::int64_t parse_integer(const std::string_view value) {
    if (value.empty()) invalid("integer field is empty");
    std::size_t consumed = 0;
    const auto parsed = std::stoll(std::string(value), &consumed);
    if (consumed != value.size()) invalid("integer field contains trailing data");
    return parsed;
}

void require_cpu(const RetrieverDevice device) {
    if (device == RetrieverDevice::cuda)
        throw std::runtime_error("CUDA byte retriever backend is unavailable in this native build");
}

void check_size(const Vec& value, const std::size_t expected, const char* name) {
    if (value.size() != expected) throw std::invalid_argument(std::string(name) + " shape changed");
}

Vec zeros(const std::size_t size) { return Vec(size, 0.0); }

struct LayerTape final {
    Vec input, norm1, norm1_mean, norm1_inv;
    Vec q, k, v, probabilities, attention_context, attention_projected, after_attention;
    Vec norm2, norm2_mean, norm2_inv, feedforward_pre, feedforward_gelu, feedforward_out;
};

struct ForwardTape final {
    std::size_t batch{}, bytes{}, patches{}, dimension{};
    std::vector<std::uint8_t> byte_mask, patch_mask;
    std::vector<std::size_t> patch_denominator;
    Vec embedded_patches;
    std::vector<LayerTape> layers;
    Vec encoded, pooled, output_norm, output_norm_mean, output_norm_inv, projected, normalized;
};

struct LayerGrad final {
    Vec norm1_weight, norm1_bias, attention_in_weight, attention_in_bias;
    Vec attention_out_weight, attention_out_bias, norm2_weight, norm2_bias;
    Vec linear1_weight, linear1_bias, linear2_weight, linear2_bias;
};

struct WeightGrad final {
    Vec byte_embedding, position_embedding;
    std::vector<LayerGrad> layers;
    Vec output_norm_weight, output_norm_bias, projection_weight;
};

WeightGrad zero_gradient(const ByteRetrieverConfig& c) {
    WeightGrad g;
    g.byte_embedding = zeros(257 * c.model_dim);
    g.position_embedding = zeros((std::max(c.max_query_bytes, c.max_document_bytes) /
                                  c.patch_size) * c.model_dim);
    g.layers.resize(c.layers);
    for (auto& layer : g.layers) {
        layer.norm1_weight = zeros(c.model_dim); layer.norm1_bias = zeros(c.model_dim);
        layer.attention_in_weight = zeros(3 * c.model_dim * c.model_dim);
        layer.attention_in_bias = zeros(3 * c.model_dim);
        layer.attention_out_weight = zeros(c.model_dim * c.model_dim);
        layer.attention_out_bias = zeros(c.model_dim);
        layer.norm2_weight = zeros(c.model_dim); layer.norm2_bias = zeros(c.model_dim);
        layer.linear1_weight = zeros(c.ffn_dim * c.model_dim);
        layer.linear1_bias = zeros(c.ffn_dim);
        layer.linear2_weight = zeros(c.model_dim * c.ffn_dim);
        layer.linear2_bias = zeros(c.model_dim);
    }
    g.output_norm_weight = zeros(c.model_dim); g.output_norm_bias = zeros(c.model_dim);
    g.projection_weight = zeros(c.embedding_dim * c.model_dim);
    return g;
}

void layer_norm_forward(const Vec& input, const std::size_t rows, const std::size_t dim,
                        const Vec& weight, const Vec& bias, Vec& output,
                        Vec& means, Vec& inverses) {
    output.resize(rows * dim); means.resize(rows); inverses.resize(rows);
    for (std::size_t row = 0; row != rows; ++row) {
        const auto base = row * dim;
        double mean = 0.0;
        for (std::size_t column = 0; column != dim; ++column) mean += input[base + column];
        mean /= static_cast<double>(dim);
        double variance = 0.0;
        for (std::size_t column = 0; column != dim; ++column) {
            const auto delta = input[base + column] - mean; variance += delta * delta;
        }
        variance /= static_cast<double>(dim);
        const auto inverse = 1.0 / std::sqrt(variance + layer_norm_epsilon);
        means[row] = mean; inverses[row] = inverse;
        for (std::size_t column = 0; column != dim; ++column)
            output[base + column] = (input[base + column] - mean) * inverse * weight[column] + bias[column];
    }
}

Vec layer_norm_backward(const Vec& input, const Vec& output_gradient,
                        const std::size_t rows, const std::size_t dim,
                        const Vec& weight, const Vec& means, const Vec& inverses,
                        Vec& weight_gradient, Vec& bias_gradient) {
    Vec result(rows * dim);
    for (std::size_t row = 0; row != rows; ++row) {
        const auto base = row * dim;
        double sum = 0.0, normalized_sum = 0.0;
        for (std::size_t column = 0; column != dim; ++column) {
            const auto normalized = (input[base + column] - means[row]) * inverses[row];
            const auto scaled = output_gradient[base + column] * weight[column];
            sum += scaled; normalized_sum += scaled * normalized;
            weight_gradient[column] += output_gradient[base + column] * normalized;
            bias_gradient[column] += output_gradient[base + column];
        }
        for (std::size_t column = 0; column != dim; ++column) {
            const auto normalized = (input[base + column] - means[row]) * inverses[row];
            const auto scaled = output_gradient[base + column] * weight[column];
            result[base + column] = inverses[row] *
                (scaled - sum / static_cast<double>(dim) -
                 normalized * normalized_sum / static_cast<double>(dim));
        }
    }
    return result;
}

void linear_forward(const Vec& input, const std::size_t rows, const std::size_t in,
                    const Vec& weight, const Vec* bias, const std::size_t out, Vec& output) {
    output.assign(rows * out, 0.0);
    for (std::size_t row = 0; row != rows; ++row)
        for (std::size_t target = 0; target != out; ++target) {
            double value = bias ? (*bias)[target] : 0.0;
            for (std::size_t source = 0; source != in; ++source)
                value += input[row * in + source] * weight[target * in + source];
            output[row * out + target] = value;
        }
}

Vec linear_backward(const Vec& input, const Vec& output_gradient,
                    const std::size_t rows, const std::size_t in, const Vec& weight,
                    const std::size_t out, Vec& weight_gradient, Vec* bias_gradient) {
    Vec result(rows * in, 0.0);
    for (std::size_t row = 0; row != rows; ++row)
        for (std::size_t target = 0; target != out; ++target) {
            const auto gradient = output_gradient[row * out + target];
            if (bias_gradient) (*bias_gradient)[target] += gradient;
            for (std::size_t source = 0; source != in; ++source) {
                weight_gradient[target * in + source] += gradient * input[row * in + source];
                result[row * in + source] += gradient * weight[target * in + source];
            }
        }
    return result;
}

void attention_forward(const Vec& input, const std::vector<std::uint8_t>& mask,
                       const std::size_t batch, const std::size_t tokens,
                       const ByteRetrieverConfig& c, const ByteTransformerLayerWeights& w,
                       LayerTape& tape) {
    const auto d = c.model_dim, rows = batch * tokens, head_dim = d / c.attention_heads;
    Vec qkv;
    linear_forward(input, rows, d, w.attention_in_weight, &w.attention_in_bias, 3 * d, qkv);
    tape.q.resize(rows * d); tape.k.resize(rows * d); tape.v.resize(rows * d);
    for (std::size_t row = 0; row != rows; ++row)
        for (std::size_t column = 0; column != d; ++column) {
            tape.q[row*d+column]=qkv[row*3*d+column];
            tape.k[row*d+column]=qkv[row*3*d+d+column];
            tape.v[row*d+column]=qkv[row*3*d+2*d+column];
        }
    tape.probabilities.assign(batch*c.attention_heads*tokens*tokens, 0.0);
    tape.attention_context.assign(rows*d, 0.0);
    const auto scale = 1.0/std::sqrt(static_cast<double>(head_dim));
    for (std::size_t b=0;b<batch;++b) for(std::size_t h=0;h<c.attention_heads;++h)
        for(std::size_t q=0;q<tokens;++q){
            const auto has_key = std::any_of(
                mask.begin() + static_cast<std::ptrdiff_t>(b * tokens),
                mask.begin() + static_cast<std::ptrdiff_t>((b + 1) * tokens),
                [](const std::uint8_t value) { return value != 0; });
            if (!has_key) {
                for (std::size_t x = 0; x != head_dim; ++x)
                    tape.attention_context[(b * tokens + q) * d + h * head_dim + x] =
                        std::numeric_limits<float>::quiet_NaN();
                continue;
            }
            double maximum=-std::numeric_limits<double>::infinity();
            for(std::size_t k=0;k<tokens;++k){
                if(!mask[b*tokens+k]) continue;
                double score=0; for(std::size_t x=0;x<head_dim;++x){auto col=h*head_dim+x;
                    score+=tape.q[(b*tokens+q)*d+col]*tape.k[(b*tokens+k)*d+col];}
                score*=scale; maximum=std::max(maximum,score);
                tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k]=score;
            }
            double denominator=0;
            for(std::size_t k=0;k<tokens;++k) if(mask[b*tokens+k]){
                auto& p=tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k];
                p=static_cast<float>(std::exp(p-maximum));denominator+=p;
            }
            for(std::size_t k=0;k<tokens;++k) if(mask[b*tokens+k]){
                auto& p=tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k];p/=denominator;
                for(std::size_t x=0;x<head_dim;++x){auto col=h*head_dim+x;
                    tape.attention_context[(b*tokens+q)*d+col]+=p*tape.v[(b*tokens+k)*d+col];}
            }
        }
    linear_forward(tape.attention_context, rows, d, w.attention_out_weight,
                   &w.attention_out_bias, d, tape.attention_projected);
}

Vec attention_backward(const Vec& gradient, const std::vector<std::uint8_t>& mask,
                       const std::size_t batch, const std::size_t tokens,
                       const ByteRetrieverConfig& c, const ByteTransformerLayerWeights& w,
                       const LayerTape& tape, LayerGrad& g) {
    const auto d=c.model_dim, rows=batch*tokens, hd=d/c.attention_heads;
    auto context_gradient=linear_backward(tape.attention_context,gradient,rows,d,
        w.attention_out_weight,d,g.attention_out_weight,&g.attention_out_bias);
    Vec qg(rows*d,0),kg(rows*d,0),vg(rows*d,0); const auto scale=1.0/std::sqrt(static_cast<double>(hd));
    for(std::size_t b=0;b<batch;++b)for(std::size_t h=0;h<c.attention_heads;++h)
        for(std::size_t q=0;q<tokens;++q){
            std::vector<double> probability_gradient(tokens,0.0);
            for(std::size_t k=0;k<tokens;++k)if(mask[b*tokens+k])
                for(std::size_t x=0;x<hd;++x){auto col=h*hd+x;
                    probability_gradient[k]+=context_gradient[(b*tokens+q)*d+col]*tape.v[(b*tokens+k)*d+col];
                    vg[(b*tokens+k)*d+col]+=tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k]*context_gradient[(b*tokens+q)*d+col];}
            double dot=0;for(std::size_t k=0;k<tokens;++k)dot+=probability_gradient[k]*tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k];
            for(std::size_t k=0;k<tokens;++k)if(mask[b*tokens+k]){
                const auto sg=(probability_gradient[k]-dot)*tape.probabilities[((b*c.attention_heads+h)*tokens+q)*tokens+k]*scale;
                for(std::size_t x=0;x<hd;++x){auto col=h*hd+x;
                    qg[(b*tokens+q)*d+col]+=sg*tape.k[(b*tokens+k)*d+col];
                    kg[(b*tokens+k)*d+col]+=sg*tape.q[(b*tokens+q)*d+col];}
            }
        }
    Vec qkvg(rows*3*d);for(std::size_t row=0;row<rows;++row)for(std::size_t x=0;x<d;++x){
        qkvg[row*3*d+x]=qg[row*d+x];qkvg[row*3*d+d+x]=kg[row*d+x];qkvg[row*3*d+2*d+x]=vg[row*d+x];}
    return linear_backward(tape.norm1,qkvg,rows,d,w.attention_in_weight,3*d,
                           g.attention_in_weight,&g.attention_in_bias);
}

double gelu(const double x){return 0.5*x*(1.0+std::erf(x/std::sqrt(2.0)));}
double gelu_derivative(const double x){return 0.5*(1.0+std::erf(x/std::sqrt(2.0)))+
    x*std::exp(-0.5*x*x)/std::sqrt(2.0*std::numbers::pi);}

ForwardTape forward_tape(const BytePatchRetriever& model, const ByteIdBatch& ids) {
    const auto& c=model.config();const auto& w=model.weights();
    if(ids.empty()) invalid("byte_ids batch cannot be empty");
    const auto bytes=ids.front().size();if(!bytes||bytes%c.patch_size)invalid("byte dimension must be divisible by patch_size");
    for(const auto& row:ids)if(row.size()!=bytes)invalid("byte_ids must have rectangular shape [batch, bytes]");
    ForwardTape t;t.batch=ids.size();t.bytes=bytes;t.patches=bytes/c.patch_size;t.dimension=c.model_dim;
    if(t.patches>std::max(c.max_query_bytes,c.max_document_bytes)/c.patch_size)invalid("byte_ids exceed position embedding capacity");
    t.byte_mask.resize(t.batch*bytes);t.patch_mask.resize(t.batch*t.patches);t.patch_denominator.resize(t.batch*t.patches);
    t.embedded_patches.assign(t.batch*t.patches*c.model_dim,0.0);
    for(std::size_t b=0;b<t.batch;++b)for(std::size_t p=0;p<t.patches;++p){
        std::size_t count=0;for(std::size_t x=0;x<c.patch_size;++x){const auto id=ids[b][p*c.patch_size+x];
            if(id<0||id>256)invalid("byte_ids values must be within [0, 256]"); if(id==0)continue;
            t.byte_mask[b*bytes+p*c.patch_size+x]=1;++count;
            for(std::size_t d=0;d<c.model_dim;++d)t.embedded_patches[(b*t.patches+p)*c.model_dim+d]+=w.byte_embedding[static_cast<std::size_t>(id)*c.model_dim+d];}
        t.patch_mask[b*t.patches+p]=static_cast<std::uint8_t>(count!=0);t.patch_denominator[b*t.patches+p]=std::max<std::size_t>(count,1);
        for(std::size_t d=0;d<c.model_dim;++d)t.embedded_patches[(b*t.patches+p)*c.model_dim+d]=
            t.embedded_patches[(b*t.patches+p)*c.model_dim+d]/static_cast<double>(std::max<std::size_t>(count,1))+w.position_embedding[p*c.model_dim+d];
    }
    Vec current=t.embedded_patches;const auto rows=t.batch*t.patches;
    for(std::size_t layer=0;layer<c.layers;++layer){const auto& lw=w.encoder_layers[layer];LayerTape lt;lt.input=current;
        layer_norm_forward(current,rows,c.model_dim,lw.norm1_weight,lw.norm1_bias,lt.norm1,lt.norm1_mean,lt.norm1_inv);
        attention_forward(lt.norm1,t.patch_mask,t.batch,t.patches,c,lw,lt);lt.after_attention.resize(current.size());
        for(std::size_t i=0;i<current.size();++i)lt.after_attention[i]=current[i]+lt.attention_projected[i];
        layer_norm_forward(lt.after_attention,rows,c.model_dim,lw.norm2_weight,lw.norm2_bias,lt.norm2,lt.norm2_mean,lt.norm2_inv);
        linear_forward(lt.norm2,rows,c.model_dim,lw.linear1_weight,&lw.linear1_bias,c.ffn_dim,lt.feedforward_pre);
        lt.feedforward_gelu.resize(lt.feedforward_pre.size());std::ranges::transform(lt.feedforward_pre,lt.feedforward_gelu.begin(),gelu);
        linear_forward(lt.feedforward_gelu,rows,c.ffn_dim,lw.linear2_weight,&lw.linear2_bias,c.model_dim,lt.feedforward_out);
        current=lt.after_attention;for(std::size_t i=0;i<current.size();++i)current[i]+=lt.feedforward_out[i];t.layers.push_back(std::move(lt));}
    t.encoded=current;t.pooled.assign(t.batch*c.model_dim,0.0);
    for(std::size_t b=0;b<t.batch;++b){std::size_t count=0;for(std::size_t p=0;p<t.patches;++p)if(t.patch_mask[b*t.patches+p]){++count;for(std::size_t d=0;d<c.model_dim;++d)t.pooled[b*c.model_dim+d]+=current[(b*t.patches+p)*c.model_dim+d];}
        for(std::size_t d=0;d<c.model_dim;++d)t.pooled[b*c.model_dim+d]/=static_cast<double>(std::max<std::size_t>(count,1));}
    layer_norm_forward(t.pooled,t.batch,c.model_dim,w.output_norm_weight,w.output_norm_bias,t.output_norm,t.output_norm_mean,t.output_norm_inv);
    linear_forward(t.output_norm,t.batch,c.model_dim,w.projection_weight,nullptr,c.embedding_dim,t.projected);
    t.normalized=t.projected;for(std::size_t b=0;b<t.batch;++b){double norm=0;for(std::size_t e=0;e<c.embedding_dim;++e)norm+=t.projected[b*c.embedding_dim+e]*t.projected[b*c.embedding_dim+e];norm=std::max(std::sqrt(norm),1e-12);
        for(std::size_t e=0;e<c.embedding_dim;++e)t.normalized[b*c.embedding_dim+e]/=norm;}
    return t;
}

void add_gradient(WeightGrad& target,const WeightGrad& source){
    const auto add=[](Vec& a,const Vec& b){for(std::size_t i=0;i<a.size();++i)a[i]+=b[i];};
    add(target.byte_embedding,source.byte_embedding);add(target.position_embedding,source.position_embedding);
    for(std::size_t i=0;i<target.layers.size();++i){auto&a=target.layers[i];const auto&b=source.layers[i];
#define ADD_FIELD(name) add(a.name,b.name)
        ADD_FIELD(norm1_weight);ADD_FIELD(norm1_bias);ADD_FIELD(attention_in_weight);ADD_FIELD(attention_in_bias);ADD_FIELD(attention_out_weight);ADD_FIELD(attention_out_bias);ADD_FIELD(norm2_weight);ADD_FIELD(norm2_bias);ADD_FIELD(linear1_weight);ADD_FIELD(linear1_bias);ADD_FIELD(linear2_weight);ADD_FIELD(linear2_bias);
#undef ADD_FIELD
    }add(target.output_norm_weight,source.output_norm_weight);add(target.output_norm_bias,source.output_norm_bias);add(target.projection_weight,source.projection_weight);
}

WeightGrad backward(const BytePatchRetriever& model,const ByteIdBatch& ids,const ForwardTape&t,const Vec& normalized_gradient){
    const auto&c=model.config();const auto&w=model.weights();auto g=zero_gradient(c);Vec projected_gradient(t.projected.size());
    for(std::size_t b=0;b<t.batch;++b){double norm=0,dot=0;for(std::size_t e=0;e<c.embedding_dim;++e){norm+=t.projected[b*c.embedding_dim+e]*t.projected[b*c.embedding_dim+e];dot+=normalized_gradient[b*c.embedding_dim+e]*t.normalized[b*c.embedding_dim+e];}norm=std::max(std::sqrt(norm),1e-12);
        for(std::size_t e=0;e<c.embedding_dim;++e)projected_gradient[b*c.embedding_dim+e]=(normalized_gradient[b*c.embedding_dim+e]-t.normalized[b*c.embedding_dim+e]*dot)/norm;}
    auto norm_gradient=linear_backward(t.output_norm,projected_gradient,t.batch,c.model_dim,w.projection_weight,c.embedding_dim,g.projection_weight,nullptr);
    auto pooled_gradient=layer_norm_backward(t.pooled,norm_gradient,t.batch,c.model_dim,w.output_norm_weight,t.output_norm_mean,t.output_norm_inv,g.output_norm_weight,g.output_norm_bias);
    Vec current_gradient(t.encoded.size(),0.0);for(std::size_t b=0;b<t.batch;++b){std::size_t count=0;for(std::size_t p=0;p<t.patches;++p)count+=t.patch_mask[b*t.patches+p];
        for(std::size_t p=0;p<t.patches;++p)if(t.patch_mask[b*t.patches+p])for(std::size_t d=0;d<c.model_dim;++d)current_gradient[(b*t.patches+p)*c.model_dim+d]+=pooled_gradient[b*c.model_dim+d]/static_cast<double>(std::max<std::size_t>(count,1));}
    const auto rows=t.batch*t.patches;
    for(std::size_t reverse=0;reverse<c.layers;++reverse){const auto layer=c.layers-1-reverse;const auto&lt=t.layers[layer];const auto&lw=w.encoder_layers[layer];auto&lg=g.layers[layer];
        Vec gelu_gradient=linear_backward(lt.feedforward_gelu,current_gradient,rows,c.ffn_dim,lw.linear2_weight,c.model_dim,lg.linear2_weight,&lg.linear2_bias);
        for(std::size_t i=0;i<gelu_gradient.size();++i)gelu_gradient[i]*=gelu_derivative(lt.feedforward_pre[i]);
        auto norm2_gradient=linear_backward(lt.norm2,gelu_gradient,rows,c.model_dim,lw.linear1_weight,c.ffn_dim,lg.linear1_weight,&lg.linear1_bias);
        auto after_attention_gradient=layer_norm_backward(lt.after_attention,norm2_gradient,rows,c.model_dim,lw.norm2_weight,lt.norm2_mean,lt.norm2_inv,lg.norm2_weight,lg.norm2_bias);
        for(std::size_t i=0;i<after_attention_gradient.size();++i)after_attention_gradient[i]+=current_gradient[i];
        auto norm1_gradient=attention_backward(after_attention_gradient,t.patch_mask,t.batch,t.patches,c,lw,lt,lg);
        auto input_gradient=layer_norm_backward(lt.input,norm1_gradient,rows,c.model_dim,lw.norm1_weight,lt.norm1_mean,lt.norm1_inv,lg.norm1_weight,lg.norm1_bias);
        for(std::size_t i=0;i<input_gradient.size();++i)input_gradient[i]+=after_attention_gradient[i];current_gradient=std::move(input_gradient);}
    for(std::size_t b=0;b<t.batch;++b)for(std::size_t p=0;p<t.patches;++p)for(std::size_t d=0;d<c.model_dim;++d){const auto value=current_gradient[(b*t.patches+p)*c.model_dim+d];g.position_embedding[p*c.model_dim+d]+=value;const auto denominator=static_cast<double>(t.patch_denominator[b*t.patches+p]);
        for(std::size_t x=0;x<c.patch_size;++x){const auto id=ids[b][p*c.patch_size+x];if(id!=0)g.byte_embedding[static_cast<std::size_t>(id)*c.model_dim+d]+=value/denominator;}}
    return g;
}

void validate_weights(const ByteRetrieverConfig& c,const BytePatchRetrieverWeights&w){
    check_size(w.byte_embedding,257*c.model_dim,"byte_embedding");check_size(w.position_embedding,(std::max(c.max_query_bytes,c.max_document_bytes)/c.patch_size)*c.model_dim,"position_embedding");
    if(w.encoder_layers.size()!=c.layers)invalid("encoder layer count changed");for(const auto&l:w.encoder_layers){
        check_size(l.norm1_weight,c.model_dim,"norm1.weight");check_size(l.norm1_bias,c.model_dim,"norm1.bias");check_size(l.attention_in_weight,3*c.model_dim*c.model_dim,"attention.in_proj_weight");check_size(l.attention_in_bias,3*c.model_dim,"attention.in_proj_bias");check_size(l.attention_out_weight,c.model_dim*c.model_dim,"attention.out_proj.weight");check_size(l.attention_out_bias,c.model_dim,"attention.out_proj.bias");check_size(l.norm2_weight,c.model_dim,"norm2.weight");check_size(l.norm2_bias,c.model_dim,"norm2.bias");check_size(l.linear1_weight,c.ffn_dim*c.model_dim,"linear1.weight");check_size(l.linear1_bias,c.ffn_dim,"linear1.bias");check_size(l.linear2_weight,c.model_dim*c.ffn_dim,"linear2.weight");check_size(l.linear2_bias,c.model_dim,"linear2.bias");}
    check_size(w.output_norm_weight,c.model_dim,"output_norm.weight");check_size(w.output_norm_bias,c.model_dim,"output_norm.bias");check_size(w.projection_weight,c.embedding_dim*c.model_dim,"projection.weight");
}

double uniform_limit(const std::size_t fan_in){return 1.0/std::sqrt(static_cast<double>(fan_in));}
void random_uniform(Vec& values,std::mt19937_64&rng,const double limit){std::uniform_real_distribution<double>d(-limit,limit);for(auto&v:values)v=d(rng);}
void xavier_uniform(Vec& values,std::mt19937_64&rng,const std::size_t fan_in,const std::size_t fan_out){random_uniform(values,rng,std::sqrt(6.0/static_cast<double>(fan_in+fan_out)));}

BytePatchRetrieverWeights initialize_weights(const ByteRetrieverConfig&c){std::mt19937_64 rng(static_cast<std::uint64_t>(c.seed));BytePatchRetrieverWeights w;
    w.byte_embedding.resize(257*c.model_dim);std::normal_distribution<double>normal(0,1);for(auto&v:w.byte_embedding)v=normal(rng);std::fill_n(w.byte_embedding.begin(),c.model_dim,0.0);
    w.position_embedding.resize((std::max(c.max_query_bytes,c.max_document_bytes)/c.patch_size)*c.model_dim);std::normal_distribution<double>position(0,0.02);for(auto&v:w.position_embedding)v=position(rng);
    ByteTransformerLayerWeights base;base.norm1_weight=Vec(c.model_dim,1);base.norm1_bias=zeros(c.model_dim);base.attention_in_weight.resize(3*c.model_dim*c.model_dim);xavier_uniform(base.attention_in_weight,rng,c.model_dim,c.model_dim);base.attention_in_bias=zeros(3*c.model_dim);base.attention_out_weight.resize(c.model_dim*c.model_dim);random_uniform(base.attention_out_weight,rng,uniform_limit(c.model_dim));base.attention_out_bias=zeros(c.model_dim);base.norm2_weight=Vec(c.model_dim,1);base.norm2_bias=zeros(c.model_dim);base.linear1_weight.resize(c.ffn_dim*c.model_dim);random_uniform(base.linear1_weight,rng,uniform_limit(c.model_dim));base.linear1_bias.resize(c.ffn_dim);random_uniform(base.linear1_bias,rng,uniform_limit(c.model_dim));base.linear2_weight.resize(c.model_dim*c.ffn_dim);random_uniform(base.linear2_weight,rng,uniform_limit(c.ffn_dim));base.linear2_bias.resize(c.model_dim);random_uniform(base.linear2_bias,rng,uniform_limit(c.ffn_dim));w.encoder_layers.assign(c.layers,base);
    w.output_norm_weight=Vec(c.model_dim,1);w.output_norm_bias=zeros(c.model_dim);w.projection_weight.resize(c.embedding_dim*c.model_dim);random_uniform(w.projection_weight,rng,uniform_limit(c.model_dim));return w;}

void collect_parameters(BytePatchRetrieverWeights&w,WeightGrad&g,std::vector<std::pair<Vec*,Vec*>>&r){const auto add=[&](Vec&v,Vec&x){r.emplace_back(&v,&x);};add(w.byte_embedding,g.byte_embedding);add(w.position_embedding,g.position_embedding);for(std::size_t i=0;i<w.encoder_layers.size();++i){auto&a=w.encoder_layers[i];auto&b=g.layers[i];
#define PARAM(name) add(a.name,b.name)
    PARAM(norm1_weight);PARAM(norm1_bias);PARAM(attention_in_weight);PARAM(attention_in_bias);PARAM(attention_out_weight);PARAM(attention_out_bias);PARAM(norm2_weight);PARAM(norm2_bias);PARAM(linear1_weight);PARAM(linear1_bias);PARAM(linear2_weight);PARAM(linear2_bias);
#undef PARAM
    }add(w.output_norm_weight,g.output_norm_weight);add(w.output_norm_bias,g.output_norm_bias);add(w.projection_weight,g.projection_weight);}

std::string digest_hex(const architecture::DigestBytes&digest){constexpr char digits[]="0123456789abcdef";std::string result(digest.size()*2,'0');for(std::size_t i=0;i<digest.size();++i){const auto v=std::to_integer<unsigned>(digest[i]);result[i*2]=digits[v>>4];result[i*2+1]=digits[v&15];}return result;}

template<class T>void write_scalar(std::ostream&out,const T value){static_assert(std::is_trivially_copyable_v<T>);std::array<std::byte,sizeof(T)>bytes{};std::memcpy(bytes.data(),&value,sizeof value);if constexpr(std::endian::native==std::endian::big)std::ranges::reverse(bytes);out.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!out)throw std::runtime_error("byte retriever checkpoint write failed");}
template<class T>T read_scalar(std::istream&in){std::array<std::byte,sizeof(T)>bytes{};in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));if(!in)throw std::runtime_error("byte retriever checkpoint is truncated");if constexpr(std::endian::native==std::endian::big)std::ranges::reverse(bytes);T value{};std::memcpy(&value,bytes.data(),sizeof value);return value;}
void write_string(std::ostream&out,const std::string_view value){write_scalar<std::uint64_t>(out,value.size());out.write(value.data(),static_cast<std::streamsize>(value.size()));if(!out)throw std::runtime_error("byte retriever checkpoint write failed");}
std::string read_string(std::istream&in){const auto size=read_scalar<std::uint64_t>(in);if(size>1U<<20)invalid("byte retriever checkpoint string limit exceeded");std::string result(static_cast<std::size_t>(size),'\0');in.read(result.data(),static_cast<std::streamsize>(result.size()));if(!in)throw std::runtime_error("byte retriever checkpoint is truncated");return result;}
void write_vector(std::ostream&out,const Vec&v){write_scalar<std::uint64_t>(out,v.size());for(const auto x:v)write_scalar(out,x);}
Vec read_vector(std::istream&in,const std::size_t expected){const auto size=read_scalar<std::uint64_t>(in);if(size!=expected)invalid("byte retriever checkpoint tensor shape changed");Vec result(expected);for(auto&x:result)x=read_scalar<float>(in);return result;}

void write_config(std::ostream&out,const ByteRetrieverConfig&c){write_scalar(out,c.seed);write_scalar<std::uint64_t>(out,c.patch_size);write_scalar<std::uint64_t>(out,c.model_dim);write_scalar<std::uint64_t>(out,c.attention_heads);write_scalar<std::uint64_t>(out,c.ffn_dim);write_scalar<std::uint64_t>(out,c.layers);write_scalar<std::uint64_t>(out,c.embedding_dim);write_scalar<std::uint64_t>(out,c.max_query_bytes);write_scalar<std::uint64_t>(out,c.max_document_bytes);write_scalar<std::uint64_t>(out,c.batch_size);write_scalar<std::uint64_t>(out,c.train_steps);write_scalar(out,c.learning_rate);write_scalar(out,c.temperature);}
ByteRetrieverConfig read_config(std::istream&in){ByteRetrieverConfig c;c.seed=read_scalar<std::int64_t>(in);c.patch_size=read_scalar<std::uint64_t>(in);c.model_dim=read_scalar<std::uint64_t>(in);c.attention_heads=read_scalar<std::uint64_t>(in);c.ffn_dim=read_scalar<std::uint64_t>(in);c.layers=read_scalar<std::uint64_t>(in);c.embedding_dim=read_scalar<std::uint64_t>(in);c.max_query_bytes=read_scalar<std::uint64_t>(in);c.max_document_bytes=read_scalar<std::uint64_t>(in);c.batch_size=read_scalar<std::uint64_t>(in);c.train_steps=read_scalar<std::uint64_t>(in);c.learning_rate=read_scalar<double>(in);c.temperature=read_scalar<double>(in);c.validate();return c;}

void write_weights(std::ostream&out,const BytePatchRetrieverWeights&w){write_vector(out,w.byte_embedding);write_vector(out,w.position_embedding);write_scalar<std::uint64_t>(out,w.encoder_layers.size());for(const auto&l:w.encoder_layers){write_vector(out,l.norm1_weight);write_vector(out,l.norm1_bias);write_vector(out,l.attention_in_weight);write_vector(out,l.attention_in_bias);write_vector(out,l.attention_out_weight);write_vector(out,l.attention_out_bias);write_vector(out,l.norm2_weight);write_vector(out,l.norm2_bias);write_vector(out,l.linear1_weight);write_vector(out,l.linear1_bias);write_vector(out,l.linear2_weight);write_vector(out,l.linear2_bias);}write_vector(out,w.output_norm_weight);write_vector(out,w.output_norm_bias);write_vector(out,w.projection_weight);}
BytePatchRetrieverWeights read_weights(std::istream&in,const ByteRetrieverConfig&c){BytePatchRetrieverWeights w;w.byte_embedding=read_vector(in,257*c.model_dim);w.position_embedding=read_vector(in,(std::max(c.max_query_bytes,c.max_document_bytes)/c.patch_size)*c.model_dim);if(read_scalar<std::uint64_t>(in)!=c.layers)invalid("byte retriever checkpoint layer count changed");w.encoder_layers.resize(c.layers);for(auto&l:w.encoder_layers){l.norm1_weight=read_vector(in,c.model_dim);l.norm1_bias=read_vector(in,c.model_dim);l.attention_in_weight=read_vector(in,3*c.model_dim*c.model_dim);l.attention_in_bias=read_vector(in,3*c.model_dim);l.attention_out_weight=read_vector(in,c.model_dim*c.model_dim);l.attention_out_bias=read_vector(in,c.model_dim);l.norm2_weight=read_vector(in,c.model_dim);l.norm2_bias=read_vector(in,c.model_dim);l.linear1_weight=read_vector(in,c.ffn_dim*c.model_dim);l.linear1_bias=read_vector(in,c.ffn_dim);l.linear2_weight=read_vector(in,c.model_dim*c.ffn_dim);l.linear2_bias=read_vector(in,c.model_dim);}w.output_norm_weight=read_vector(in,c.model_dim);w.output_norm_bias=read_vector(in,c.model_dim);w.projection_weight=read_vector(in,c.embedding_dim*c.model_dim);return w;}

std::string json_escape(const std::string_view value){std::ostringstream out;out<<'"';for(const unsigned char c:value){switch(c){case '"':out<<"\\\"";break;case '\\':out<<"\\\\";break;case '\b':out<<"\\b";break;case '\f':out<<"\\f";break;case '\n':out<<"\\n";break;case '\r':out<<"\\r";break;case '\t':out<<"\\t";break;default:if(c<0x20)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;else out<<static_cast<char>(c);}}out<<'"';return out.str();}

std::pair<ByteRetrieverConfig,BytePatchRetrieverWeights> load_checkpoint(const std::filesystem::path&path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot open byte retriever checkpoint");if(read_string(in)!=checkpoint_schema)invalid("unsupported byte retriever checkpoint schema");auto c=read_config(in);auto w=read_weights(in,c);if(in.peek()!=std::char_traits<char>::eof())invalid("byte retriever checkpoint has trailing bytes");validate_weights(c,w);return {c,std::move(w)};}

RetrieverMetrics expected_report_metrics(const std::filesystem::path&path){const auto text=read_text(path);std::pmr::monotonic_buffer_resource memory;const auto root=transport::parse_json(text,memory);const auto&metrics=root.at("trained_dev").at("metrics");const auto number=[](const transport::Json&v){std::size_t used=0;const auto result=std::stod(std::string(v.scalar),&used);if(used!=v.scalar.size())invalid("expected report metric is invalid");return result;};return {number(metrics.at("mrr_at_10")),number(metrics.at("ndcg_at_10")),number(metrics.at("recall_at_10")),number(metrics.at("recall_at_100"))};}

}  // namespace

void ByteRetrieverConfig::validate() const {
    if(!patch_size||!model_dim||!attention_heads||!ffn_dim||!embedding_dim||!max_query_bytes||!max_document_bytes||!batch_size||!train_steps)invalid("retriever configuration values must be positive");
    if(model_dim%attention_heads)invalid("model_dim must be divisible by attention_heads");
    if(max_query_bytes%patch_size)invalid("max_query_bytes must be divisible by patch_size");
    if(max_document_bytes%patch_size)invalid("max_document_bytes must be divisible by patch_size");
}

BytePatchRetriever::BytePatchRetriever(ByteRetrieverConfig config):config_(std::move(config)){config_.validate();weights_=initialize_weights(config_);validate_weights(config_,weights_);}
BytePatchRetriever::BytePatchRetriever(ByteRetrieverConfig config,BytePatchRetrieverWeights weights):config_(std::move(config)),weights_(std::move(weights)){config_.validate();validate_weights(config_,weights_);}
FloatMatrix BytePatchRetriever::forward(const ByteIdBatch&byte_ids)const{const auto tape=forward_tape(*this,byte_ids);FloatMatrix result(tape.batch,std::vector<double>(config_.embedding_dim));for(std::size_t b=0;b<tape.batch;++b)std::copy_n(tape.normalized.begin()+static_cast<std::ptrdiff_t>(b*config_.embedding_dim),config_.embedding_dim,result[b].begin());return result;}
std::uint64_t BytePatchRetriever::parameter_count()const noexcept{std::uint64_t count=weights_.byte_embedding.size()+weights_.position_embedding.size()+weights_.output_norm_weight.size()+weights_.output_norm_bias.size()+weights_.projection_weight.size();for(const auto&l:weights_.encoder_layers)count+=l.norm1_weight.size()+l.norm1_bias.size()+l.attention_in_weight.size()+l.attention_in_bias.size()+l.attention_out_weight.size()+l.attention_out_bias.size()+l.norm2_weight.size()+l.norm2_bias.size()+l.linear1_weight.size()+l.linear1_bias.size()+l.linear2_weight.size()+l.linear2_bias.size();return count;}

ByteIdBatch encode_texts(const std::vector<std::string>&texts,const std::size_t max_bytes,const std::size_t patch_size){if(texts.empty())invalid("texts cannot be empty");if(!max_bytes||!patch_size)invalid("max_bytes and patch_size must be positive");std::size_t used=1;for(const auto&text:texts)used=std::max(used,std::min(text.size(),max_bytes));const auto padded=std::min(max_bytes,((used+patch_size-1)/patch_size)*patch_size);ByteIdBatch result(texts.size(),std::vector<std::int64_t>(padded));for(std::size_t row=0;row<texts.size();++row)for(std::size_t index=0;index<std::min(texts[row].size(),max_bytes);++index)result[row][index]=static_cast<unsigned char>(texts[row][index])+1;return result;}

RetrievalSplit load_retrieval_split(const std::filesystem::path&topics_path,const std::filesystem::path&qrels_path,const std::filesystem::path&corpus_path){RetrievalSplit result;for(const auto line:lines(read_text(topics_path))){const auto at=line.find('\t');if(at==std::string_view::npos)invalid("topic row must contain a tab");result.topics[std::string(line.substr(0,at))]=std::string(line.substr(at+1));}
    for(const auto line:lines(read_text(qrels_path))){const auto columns=four_columns(line);auto&target=parse_integer(columns[3])>0?result.positives:result.negatives;target[std::string(columns[0])].insert(std::string(columns[2]));}
    for(const auto line:lines(read_text(corpus_path))){std::pmr::monotonic_buffer_resource memory;const auto record=transport::parse_json(line,memory);const auto id=std::string(record.at("docid").string());if(result.documents.contains(id))throw std::invalid_argument("duplicate document: "+id);result.documents.emplace(id,std::string(record.at("title").string())+'\n'+std::string(record.at("text").string()));}
    std::size_t missing=0;std::set<std::string,std::less<>> seen;for(const auto*table:{&result.positives,&result.negatives})for(const auto&[_,ids]:*table)for(const auto&id:ids)if(!result.documents.contains(id)&&seen.insert(id).second)++missing;if(missing)throw std::invalid_argument("qrels reference missing documents: "+std::to_string(missing));return result;}

std::string_view retriever_device_name(const RetrieverDevice device)noexcept{return device==RetrieverDevice::cpu?"cpu":"cuda";}
RetrieverDevice resolve_retriever_device(std::string_view value,const bool cuda_available){if(value=="auto")value=cuda_available?"cuda":"cpu";if(value=="cuda"&&!cuda_available)throw std::runtime_error("CUDA was requested but is unavailable");if(value=="cuda")return RetrieverDevice::cuda;if(value=="cpu")return RetrieverDevice::cpu;invalid("retriever device must be auto, cpu, or cuda");}

RetrieverMetrics retrieval_metrics(const std::vector<std::string>&query_ids,const std::vector<std::vector<std::string>>&rankings,const std::map<std::string,std::set<std::string,std::less<>>,std::less<>>&positives){if(query_ids.size()!=rankings.size())invalid("query and ranking counts differ");std::vector<double>rr,r10,r100,ndcg;for(std::size_t i=0;i<query_ids.size();++i){const auto found=positives.find(query_ids[i]);if(found==positives.end()||found->second.empty())invalid("metric query has no positive qrels");const auto&relevant=found->second;std::optional<std::size_t>first;std::size_t hits10=0,hits100=0;double dcg=0;for(std::size_t rank=0;rank<rankings[i].size();++rank)if(relevant.contains(rankings[i][rank])){if(!first)first=rank+1;if(rank<10){++hits10;dcg+=1.0/std::log2(static_cast<double>(rank+2));}if(rank<100)++hits100;}rr.push_back(first&&*first<=10?1.0/static_cast<double>(*first):0.0);r10.push_back(static_cast<double>(hits10)/relevant.size());r100.push_back(static_cast<double>(hits100)/relevant.size());double ideal=0;for(std::size_t rank=1;rank<=std::min<std::size_t>(relevant.size(),10);++rank)ideal+=1.0/std::log2(static_cast<double>(rank+1));ndcg.push_back(dcg/ideal);}
    const auto mean=[](const std::vector<double>&v){return std::accumulate(v.begin(),v.end(),0.0)/static_cast<double>(v.size());};return {rounded_six(mean(rr)),rounded_six(mean(ndcg)),rounded_six(mean(r10)),rounded_six(mean(r100))};}

RetrieverEvaluation evaluate_retriever(const BytePatchRetriever&model,const RetrievalSplit&split,const ByteRetrieverConfig&config,const RetrieverDevice device,const std::size_t top_k,const std::size_t encode_batch_size){require_cpu(device);if(!encode_batch_size)invalid("encode_batch_size must be positive");std::vector<std::string>document_ids;for(const auto&[id,_]:split.documents)document_ids.push_back(id);if(document_ids.empty())invalid("retrieval corpus cannot be empty");FloatMatrix document_embeddings;const auto corpus_started=std::chrono::steady_clock::now();for(std::size_t start=0;start<document_ids.size();start+=encode_batch_size){std::vector<std::string>texts;for(std::size_t i=start;i<std::min(start+encode_batch_size,document_ids.size());++i)texts.push_back(split.documents.at(document_ids[i]));auto encoded=model.forward(encode_texts(texts,config.max_document_bytes,config.patch_size));document_embeddings.insert(document_embeddings.end(),std::make_move_iterator(encoded.begin()),std::make_move_iterator(encoded.end()));}const auto corpus_elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-corpus_started).count();
    std::vector<std::string>query_ids,texts;for(const auto&[id,text]:split.topics){query_ids.push_back(id);texts.push_back(text);}if(query_ids.empty())invalid("retrieval query set cannot be empty");const auto query_started=std::chrono::steady_clock::now();const auto query_embeddings=model.forward(encode_texts(texts,config.max_query_bytes,config.patch_size));std::vector<std::vector<std::string>>rankings;for(const auto&q:query_embeddings){std::vector<std::pair<std::string,double>>scores;for(std::size_t d=0;d<document_ids.size();++d)scores.emplace_back(document_ids[d],std::inner_product(q.begin(),q.end(),document_embeddings[d].begin(),0.0));std::ranges::sort(scores,[](const auto&a,const auto&b){return a.second!=b.second?a.second>b.second:a.first<b.first;});if(scores.size()>top_k)scores.resize(top_k);std::vector<std::string>ranking;for(auto&[id,_]:scores)ranking.push_back(std::move(id));rankings.push_back(std::move(ranking));}const auto query_elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-query_started).count();std::size_t positives=0;for(const auto&id:query_ids)positives+=split.positives.at(id).size();return {query_ids.size(),document_ids.size(),positives,retrieval_metrics(query_ids,rankings,split.positives),rounded_six(corpus_elapsed),rounded_six(query_elapsed),rounded_six(query_elapsed*1000/query_ids.size())};}

RetrieverTrainingMetrics train_retriever(BytePatchRetriever&model,const RetrievalSplit&split,const ByteRetrieverConfig&config,const RetrieverDevice device){require_cpu(device);config.validate();if(model.config()!=config)invalid("training config does not match model config");std::vector<std::string>eligible;for(const auto&[id,_]:split.topics){const auto p=split.positives.find(id),n=split.negatives.find(id);if(p!=split.positives.end()&&!p->second.empty()&&n!=split.negatives.end()&&!n->second.empty())eligible.push_back(id);}if(eligible.empty())invalid("training split has no eligible queries");std::mt19937_64 rng(static_cast<std::uint64_t>(config.seed));std::vector<double>losses;losses.reserve(config.train_steps);std::vector<Vec>first,second;{auto initial=zero_gradient(config);std::vector<std::pair<Vec*,Vec*>>refs;collect_parameters(model.mutable_weights(),initial,refs);for(const auto&[value,_]:refs){first.emplace_back(value->size(),0);second.emplace_back(value->size(),0);}}
    const auto started=std::chrono::steady_clock::now();for(std::size_t step=1;step<=config.train_steps;++step){std::vector<std::string>query_texts,document_texts;query_texts.reserve(config.batch_size);document_texts.reserve(2*config.batch_size);for(std::size_t row=0;row<config.batch_size;++row){const auto&id=eligible[std::uniform_int_distribution<std::size_t>(0,eligible.size()-1)(rng)];query_texts.push_back(split.topics.at(id));const auto&p=split.positives.at(id);const auto&n=split.negatives.at(id);const auto pit=std::next(p.begin(),static_cast<std::ptrdiff_t>(std::uniform_int_distribution<std::size_t>(0,p.size()-1)(rng)));const auto nit=std::next(n.begin(),static_cast<std::ptrdiff_t>(std::uniform_int_distribution<std::size_t>(0,n.size()-1)(rng)));document_texts.push_back(split.documents.at(*pit));document_texts.push_back(split.documents.at(*nit));}
        auto query_ids=encode_texts(query_texts,config.max_query_bytes,config.patch_size);auto document_ids=encode_texts(document_texts,config.max_document_bytes,config.patch_size);auto qt=forward_tape(model,query_ids);auto dt=forward_tape(model,document_ids);Vec qg(qt.normalized.size(),0),dg(dt.normalized.size(),0);double loss=0;for(std::size_t row=0;row<config.batch_size;++row){std::vector<double>logits(2*config.batch_size);double maximum=-std::numeric_limits<double>::infinity();for(std::size_t doc=0;doc<logits.size();++doc){for(std::size_t e=0;e<config.embedding_dim;++e)logits[doc]+=qt.normalized[row*config.embedding_dim+e]*dt.normalized[doc*config.embedding_dim+e];logits[doc]/=config.temperature;maximum=std::max(maximum,logits[doc]);}double denominator=0;for(auto&x:logits){x=std::exp(x-maximum);denominator+=x;}const auto target=2*row;loss-=std::log(logits[target]/denominator);for(std::size_t doc=0;doc<logits.size();++doc){const auto grad=(logits[doc]/denominator-(doc==target?1.0:0.0))/static_cast<double>(config.batch_size)/config.temperature;for(std::size_t e=0;e<config.embedding_dim;++e){qg[row*config.embedding_dim+e]+=static_cast<float>(grad*dt.normalized[doc*config.embedding_dim+e]);dg[doc*config.embedding_dim+e]+=static_cast<float>(grad*qt.normalized[row*config.embedding_dim+e]);}}}loss/=config.batch_size;losses.push_back(loss);auto gradient=backward(model,query_ids,qt,qg);add_gradient(gradient,backward(model,document_ids,dt,dg));std::vector<std::pair<Vec*,Vec*>>refs;collect_parameters(model.mutable_weights(),gradient,refs);double squared=0;for(const auto&[_,grad]:refs)for(const auto x:*grad)squared+=x*x;const auto clip=std::min(1.0,1.0/(std::sqrt(squared)+1e-6));const auto beta1=0.9,beta2=0.999,eps=1e-8,decay=0.01;for(std::size_t p=0;p<refs.size();++p)for(std::size_t i=0;i<refs[p].first->size();++i){const auto grad=(*refs[p].second)[i]*clip;first[p][i]=static_cast<float>(beta1*first[p][i]+(1-beta1)*grad);second[p][i]=static_cast<float>(beta2*second[p][i]+(1-beta2)*grad*grad);const auto m=first[p][i]/(1-std::pow(beta1,static_cast<double>(step)));const auto v=second[p][i]/(1-std::pow(beta2,static_cast<double>(step)));(*refs[p].first)[i]=static_cast<float>((*refs[p].first)[i]*(1-config.learning_rate*decay)-config.learning_rate*m/(std::sqrt(v)+eps));}}
    const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();const auto tail=std::min<std::size_t>(losses.size(),100);return {eligible.size(),rounded_six(losses.front()),rounded_six(losses.back()),rounded_six(std::accumulate(losses.end()-static_cast<std::ptrdiff_t>(tail),losses.end(),0.0)/tail),rounded_six(elapsed),rounded_six(config.train_steps/elapsed),0,0};}

bool ByteRetrieverAcceptance::passed()const noexcept{return parameter_count_at_most_5m&&checkpoint_at_most_25mb&&mrr_at_10_at_least_0_10&&recall_at_100_at_least_0_50&&mrr_gain_at_least_0_05&&loss_fell;}
bool ByteRetrieverVerificationChecks::passed()const noexcept{return manifest_digest&&manifest_size&&parameter_count_at_most_5m&&mrr_at_10_at_least_0_10&&recall_at_100_at_least_0_50&&(!matches_training_report||*matches_training_report);}

std::string byte_retriever_file_sha256(const std::filesystem::path&path){std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot open file for SHA-256: "+path.string());architecture::Sha256 digest;std::array<std::byte,1U<<20>buffer{};while(in){in.read(reinterpret_cast<char*>(buffer.data()),static_cast<std::streamsize>(buffer.size()));const auto count=in.gcount();if(count>0)digest.update(std::span(buffer).first(static_cast<std::size_t>(count)));}if(!in.eof())throw std::runtime_error("file SHA-256 read failed");return digest_hex(digest.finish());}

ByteRetrieverCheckpointReceipt save_byte_retriever_checkpoint(const std::filesystem::path&path,const BytePatchRetriever&model,const ByteRetrieverConfig&config){if(model.config()!=config)invalid("checkpoint config does not match model config");if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());{std::ofstream out(path,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("cannot create byte retriever checkpoint");write_string(out,checkpoint_schema);write_config(out,config);write_weights(out,model.weights());}
    const auto digest=byte_retriever_file_sha256(path);const auto bytes=std::filesystem::file_size(path);auto manifest_path=path;manifest_path+= ".manifest.json";std::ofstream manifest(manifest_path,std::ios::binary|std::ios::trunc);if(!manifest)throw std::runtime_error("cannot create byte retriever manifest");manifest<<"{\n  \"schema_version\": "<<json_escape(manifest_schema)<<",\n  \"checkpoint\": "<<json_escape(path.filename().string())<<",\n  \"bytes\": "<<bytes<<",\n  \"sha256\": "<<json_escape(digest)<<",\n  \"source\": \"random initialization trained only on MIRACL-ko train annotations and judged corpus documents\",\n  \"external_datasets\": [\n    {\n      \"id\": \"miracl-ko-train-annotations\",\n      \"revision\": \"5be20db9509754dadad47689368639fcec739c00\",\n      \"license\": \"Apache-2.0\"\n    },\n    {\n      \"id\": \"miracl-ko-corpus\",\n      \"revision\": \"d921ec7e349ce0d28daf30b2da9da5ee698bef0d\",\n      \"license\": \"Apache-2.0 packaging; CC-BY-SA-4.0 Wikipedia text\"\n    }\n  ],\n  \"license_status\": \"trained-weight redistribution review required\",\n  \"redistribution\": \"not authorized by this manifest\"\n}\n";if(!manifest)throw std::runtime_error("byte retriever manifest write failed");return {std::filesystem::absolute(path),std::filesystem::absolute(manifest_path),bytes,digest};}

ByteRetrieverExperimentReport run_byte_retriever_experiment(const ByteRetrieverConfig&config,const RetrievalSplit&train_split,const RetrievalSplit&dev_split,const RetrieverDevice device,const std::filesystem::path&checkpoint){require_cpu(device);config.validate();BytePatchRetriever model(config);ByteRetrieverExperimentReport report;report.device=device;report.config=config;report.parameter_count=model.parameter_count();report.untrained_dev=evaluate_retriever(model,dev_split,config,device);report.training=train_retriever(model,train_split,config,device);report.trained_dev=evaluate_retriever(model,dev_split,config,device);report.checkpoint=save_byte_retriever_checkpoint(checkpoint,model,config);report.mrr_at_10_gain=rounded_six(report.trained_dev.metrics.mrr_at_10-report.untrained_dev.metrics.mrr_at_10);report.acceptance={report.parameter_count<=5'000'000,report.checkpoint.bytes<=25'000'000,report.trained_dev.metrics.mrr_at_10>=0.10,report.trained_dev.metrics.recall_at_100>=0.50,report.mrr_at_10_gain>=0.05,report.training.final_loss<report.training.initial_loss};report.limitations={"Development is evaluated for this pre-registered run and must not be used for retuning.","The model sees only fixed four-byte patches and judged candidate documents.","No recurrent workspace or operator synthesis is connected yet."};return report;}

ByteRetrieverVerificationReport verify_byte_retriever_checkpoint(const std::filesystem::path&path,const RetrievalSplit&dev_split,const RetrieverDevice device,const std::optional<std::filesystem::path>&expected_report){require_cpu(device);auto[config,weights]=load_checkpoint(path);BytePatchRetriever model(config,std::move(weights));const auto evaluation=evaluate_retriever(model,dev_split,config,device);auto manifest_path=path;manifest_path += ".manifest.json";const auto manifest_text=read_text(manifest_path);std::pmr::monotonic_buffer_resource memory;const auto manifest=transport::parse_json(manifest_text,memory);const auto digest=byte_retriever_file_sha256(path);const auto bytes=std::filesystem::file_size(path);const auto manifest_digest=manifest.at("sha256").string();const auto manifest_bytes=parse_integer(manifest.at("bytes").scalar);ByteRetrieverVerificationChecks checks{digest==manifest_digest,bytes==static_cast<std::uint64_t>(manifest_bytes),model.parameter_count()<=5'000'000,evaluation.metrics.mrr_at_10>=0.10,evaluation.metrics.recall_at_100>=0.50,std::nullopt};std::optional<RetrieverMetrics> expected;if(expected_report){expected=expected_report_metrics(*expected_report);checks.matches_training_report=evaluation.metrics==*expected;}return {"mosaic-byte-retriever-verification-v0",std::filesystem::absolute(path),std::filesystem::absolute(manifest_path),digest,bytes,device,config,evaluation,expected,checks};}


namespace {

std::string boolean(const bool value) { return value ? "true" : "false"; }
std::string number_json(const double value) {
    if (!std::isfinite(value)) invalid("byte retriever report contains a non-finite number");
    std::ostringstream out; out << std::setprecision(17) << value; return out.str();
}
std::string config_json(const ByteRetrieverConfig& c) {
    return "{\"seed\":" + std::to_string(c.seed) +
        ",\"patch_size\":" + std::to_string(c.patch_size) +
        ",\"model_dim\":" + std::to_string(c.model_dim) +
        ",\"attention_heads\":" + std::to_string(c.attention_heads) +
        ",\"ffn_dim\":" + std::to_string(c.ffn_dim) +
        ",\"layers\":" + std::to_string(c.layers) +
        ",\"embedding_dim\":" + std::to_string(c.embedding_dim) +
        ",\"max_query_bytes\":" + std::to_string(c.max_query_bytes) +
        ",\"max_document_bytes\":" + std::to_string(c.max_document_bytes) +
        ",\"batch_size\":" + std::to_string(c.batch_size) +
        ",\"train_steps\":" + std::to_string(c.train_steps) +
        ",\"learning_rate\":" + number_json(c.learning_rate) +
        ",\"temperature\":" + number_json(c.temperature) + "}";
}
std::string metrics_json(const RetrieverMetrics& m) {
    return "{\"mrr_at_10\":" + number_json(m.mrr_at_10) +
        ",\"ndcg_at_10\":" + number_json(m.ndcg_at_10) +
        ",\"recall_at_10\":" + number_json(m.recall_at_10) +
        ",\"recall_at_100\":" + number_json(m.recall_at_100) + "}";
}
std::string evaluation_json(const RetrieverEvaluation& e) {
    return "{\"counts\":{\"queries\":" + std::to_string(e.queries) +
        ",\"documents\":" + std::to_string(e.documents) +
        ",\"positive_qrels\":" + std::to_string(e.positive_qrels) +
        "},\"metrics\":" + metrics_json(e.metrics) +
        ",\"timing\":{\"corpus_encode_sec\":" + number_json(e.corpus_encode_sec) +
        ",\"query_batch_and_search_sec\":" + number_json(e.query_batch_and_search_sec) +
        ",\"query_mean_ms\":" + number_json(e.query_mean_ms) + "}}";
}
std::string training_json(const RetrieverTrainingMetrics& t) {
    return "{\"eligible_queries\":" + std::to_string(t.eligible_queries) +
        ",\"initial_loss\":" + number_json(t.initial_loss) +
        ",\"final_loss\":" + number_json(t.final_loss) +
        ",\"mean_last_100_loss\":" + number_json(t.mean_last_100_loss) +
        ",\"elapsed_sec\":" + number_json(t.elapsed_sec) +
        ",\"steps_per_sec\":" + number_json(t.steps_per_sec) +
        ",\"cuda_peak_allocated_mib\":" + number_json(t.cuda_peak_allocated_mib) +
        ",\"cuda_peak_reserved_mib\":" + number_json(t.cuda_peak_reserved_mib) + "}";
}
std::string checkpoint_json(const ByteRetrieverCheckpointReceipt& c) {
    return "{\"path\":" + json_escape(c.path.string()) +
        ",\"manifest_path\":" + json_escape(c.manifest_path.string()) +
        ",\"bytes\":" + std::to_string(c.bytes) +
        ",\"sha256\":" + json_escape(c.sha256) + "}";
}
void write_report_file(const std::filesystem::path& path, const std::string_view text) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("cannot create byte retriever report");
    stream << text << '\n';
    if (!stream) throw std::runtime_error("byte retriever report write failed");
}

}  // namespace

std::string byte_retriever_experiment_json(const ByteRetrieverExperimentReport& r) {
    std::string limitations = "[";
    for (std::size_t index = 0; index != r.limitations.size(); ++index) {
        if (index) limitations.push_back(',');
        limitations += json_escape(r.limitations[index]);
    }
    limitations.push_back(']');
    const auto& a = r.acceptance;
    return "{\"schema_version\":" + json_escape(r.schema_version) +
        ",\"scope\":" + json_escape(r.scope) +
        ",\"device\":" + json_escape(retriever_device_name(r.device)) +
        ",\"config\":" + config_json(r.config) +
        ",\"parameter_count\":" + std::to_string(r.parameter_count) +
        ",\"untrained_dev\":" + evaluation_json(r.untrained_dev) +
        ",\"training\":" + training_json(r.training) +
        ",\"trained_dev\":" + evaluation_json(r.trained_dev) +
        ",\"comparison\":{\"mrr_at_10_gain\":" + number_json(r.mrr_at_10_gain) +
        ",\"frozen_bm25_dev_mrr_at_10\":" + number_json(r.frozen_bm25_dev_mrr_at_10) +
        ",\"frozen_bm25_dev_recall_at_100\":" + number_json(r.frozen_bm25_dev_recall_at_100) +
        "},\"checkpoint\":" + checkpoint_json(r.checkpoint) +
        ",\"acceptance\":{\"parameter_count_at_most_5m\":" + boolean(a.parameter_count_at_most_5m) +
        ",\"checkpoint_at_most_25mb\":" + boolean(a.checkpoint_at_most_25mb) +
        ",\"mrr_at_10_at_least_0_10\":" + boolean(a.mrr_at_10_at_least_0_10) +
        ",\"recall_at_100_at_least_0_50\":" + boolean(a.recall_at_100_at_least_0_50) +
        ",\"mrr_gain_at_least_0_05\":" + boolean(a.mrr_gain_at_least_0_05) +
        ",\"loss_fell\":" + boolean(a.loss_fell) +
        "},\"passed\":" + boolean(r.passed()) + ",\"limitations\":" + limitations + "}";
}

std::string byte_retriever_verification_json(const ByteRetrieverVerificationReport& r) {
    const auto& c = r.checks;
    std::string expected = r.expected_metrics ? metrics_json(*r.expected_metrics) : "null";
    std::string checks = "{\"manifest_digest\":" + boolean(c.manifest_digest) +
        ",\"manifest_size\":" + boolean(c.manifest_size) +
        ",\"parameter_count_at_most_5m\":" + boolean(c.parameter_count_at_most_5m) +
        ",\"mrr_at_10_at_least_0_10\":" + boolean(c.mrr_at_10_at_least_0_10) +
        ",\"recall_at_100_at_least_0_50\":" + boolean(c.recall_at_100_at_least_0_50);
    if (c.matches_training_report)
        checks += ",\"matches_training_report\":" + boolean(*c.matches_training_report);
    checks.push_back('}');
    return "{\"schema_version\":" + json_escape(r.schema_version) +
        ",\"checkpoint\":" + json_escape(r.checkpoint.string()) +
        ",\"manifest\":" + json_escape(r.manifest.string()) +
        ",\"sha256\":" + json_escape(r.sha256) +
        ",\"bytes\":" + std::to_string(r.bytes) +
        ",\"device\":" + json_escape(retriever_device_name(r.device)) +
        ",\"config\":" + config_json(r.config) +
        ",\"evaluation\":" + evaluation_json(r.evaluation) +
        ",\"expected_metrics\":" + expected +
        ",\"checks\":" + checks + ",\"passed\":" + boolean(r.passed()) + "}";
}

int run_byte_retriever_cli(const std::span<const std::string_view> arguments,
                           std::ostream& output, std::ostream& errors) {
    try {
        std::string device_value = "auto";
        std::int64_t seed = 41;
        std::size_t train_steps = 1'000, batch_size = 32;
        std::filesystem::path source_root = "data/mosaic_sources_v1";
        std::filesystem::path checkpoint = "outputs/mosaic_byte_retriever_v0.pt";
        std::optional<std::filesystem::path> verify, expected;
        std::filesystem::path report_path = "outputs/mosaic_byte_retriever_v0.json";
        const auto value = [&](std::size_t& index) -> std::string_view {
            if (++index == arguments.size()) invalid("byte retriever option is missing a value");
            return arguments[index];
        };
        for (std::size_t index = 0; index != arguments.size(); ++index) {
            const auto option = arguments[index];
            if (option == "--device") device_value = value(index);
            else if (option == "--seed") seed = parse_integer(value(index));
            else if (option == "--train-steps") {
                const auto parsed = parse_integer(value(index));
                if (parsed < 0) invalid("--train-steps must be nonnegative");
                train_steps = static_cast<std::size_t>(parsed);
            } else if (option == "--batch-size") {
                const auto parsed = parse_integer(value(index));
                if (parsed < 0) invalid("--batch-size must be nonnegative");
                batch_size = static_cast<std::size_t>(parsed);
            }
            else if (option == "--source-root") source_root = value(index);
            else if (option == "--checkpoint") checkpoint = value(index);
            else if (option == "--verify-checkpoint") verify = value(index);
            else if (option == "--expected-report") expected = value(index);
            else if (option == "--output") report_path = value(index);
            else invalid("unknown byte retriever command-line option");
        }
        const auto raw = source_root / "raw";
        const auto dev = load_retrieval_split(
            raw / "miracl-ko-dev-annotations" / "topics.ko.dev.tsv",
            raw / "miracl-ko-dev-annotations" / "qrels.ko.dev.tsv",
            source_root / "miracl_ko_dev_pilot" / "corpus.jsonl");
        const auto device = resolve_retriever_device(device_value, false);
        std::string rendered;
        bool passed = false;
        if (verify) {
            const auto report = verify_byte_retriever_checkpoint(*verify, dev, device, expected);
            rendered = byte_retriever_verification_json(report); passed = report.passed();
        } else {
            ByteRetrieverConfig config; config.seed = seed; config.train_steps = train_steps;
            config.batch_size = batch_size; config.validate();
            const auto train = load_retrieval_split(
                raw / "miracl-ko-train-annotations" / "topics.ko.train.tsv",
                raw / "miracl-ko-train-annotations" / "qrels.ko.train.tsv",
                source_root / "miracl_ko_train" / "corpus.jsonl");
            const auto report = run_byte_retriever_experiment(config, train, dev, device, checkpoint);
            rendered = byte_retriever_experiment_json(report); passed = report.passed();
        }
        output << rendered << '\n'; write_report_file(report_path, rendered); return passed ? 0 : 1;
    } catch (const std::exception& error) {
        errors << error.what() << '\n'; return 2;
    }
}

}  // namespace swegca::world
