#include "world/mosaic_omni_text_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {

constexpr std::size_t vocab = static_cast<std::size_t>(mosaic_vocab_size);
const std::set<std::string, std::less<>> cross_modes{
    "token-cross", "contextual-cross", "consistency-cross", "body-cross", "core-body-cross",
    "core-compact-body-cross", "core-projected-compact-body-cross",
    "core-lexical-compact-body-cross", "core-lexical-consistency-cross"};
const std::set<std::string, std::less<>> core_modes{
    "core-body-cross", "core-compact-body-cross", "core-projected-compact-body-cross",
    "core-lexical-compact-body-cross", "core-lexical-consistency-cross"};
const std::set<std::string, std::less<>> compact_modes{
    "core-compact-body-cross", "core-projected-compact-body-cross", "core-lexical-compact-body-cross"};
const std::set<std::string, std::less<>> projected_modes{
    "core-projected-compact-body-cross", "core-lexical-compact-body-cross", "core-lexical-consistency-cross"};
const std::set<std::string, std::less<>> lexical_modes{
    "core-lexical-compact-body-cross", "core-lexical-consistency-cross"};
const std::set<std::string, std::less<>> body_modes{
    "consistency-cross", "body-cross", "core-body-cross", "core-compact-body-cross",
    "core-projected-compact-body-cross", "core-lexical-compact-body-cross", "core-lexical-consistency-cross"};

void rectangular(const MosaicTokenBatch& rows, const std::size_t batch, const char* name) {
    if (rows.size() != batch || rows.empty() || rows.front().empty())
        throw std::invalid_argument(std::string(name) + " must be a BOS-prefixed batch matching input_ids");
    for (const auto& row : rows) if (row.size() != rows.front().size() || row.front() != mosaic_bos_id)
        throw std::invalid_argument(std::string(name) + " must be a BOS-prefixed batch matching input_ids");
}
void linear_shape(const TextAdapterLinearWeights& w, const std::size_t in, const std::size_t out,
                  const bool bias = true) {
    if (w.weight.size() != in * out || (bias ? w.bias.size() != out : !w.bias.empty()))
        throw std::invalid_argument("text pipeline linear weight shape mismatch");
}
void norm_shape(const TextAdapterNormWeights& w, const std::size_t dim) {
    if (w.weight.size() != dim || w.bias.size() != dim)
        throw std::invalid_argument("text pipeline norm weight shape mismatch");
}
Tensor linear(const Tensor& x, const TextAdapterLinearWeights& w, const std::size_t out) {
    const auto shape = x.shape(); if (shape.empty()) throw std::invalid_argument("linear input rank mismatch");
    const auto in = static_cast<std::size_t>(shape.back()), rows = x.values().size() / in;
    linear_shape(w, in, out, !w.bias.empty()); std::vector<double> values(rows * out);
    for (std::size_t row = 0; row < rows; ++row) for (std::size_t o = 0; o < out; ++o) {
        double value = w.bias.empty() ? 0.0 : w.bias[o];
        for (std::size_t i = 0; i < in; ++i) value += x.values()[row * in + i] * w.weight[o * in + i];
        values[row * out + o] = value;
    }
    std::vector<std::uint64_t> result_shape(shape.begin(), shape.end()); result_shape.back() = out;
    return Tensor(x.dtype(), std::move(result_shape), std::move(values), std::string(x.device()));
}
Tensor norm(const Tensor& x, const TextAdapterNormWeights& w) {
    const auto shape = x.shape(); const auto dim = static_cast<std::size_t>(shape.back()); norm_shape(w, dim);
    const auto rows = x.values().size() / dim; std::vector<double> values(x.values().size());
    for (std::size_t row = 0; row < rows; ++row) {
        double mean = 0.0; for (std::size_t d = 0; d < dim; ++d) mean += x.values()[row * dim + d]; mean /= dim;
        double var = 0.0; for (std::size_t d = 0; d < dim; ++d) { const auto v=x.values()[row*dim+d]-mean; var+=v*v; }
        const auto inverse = 1.0 / std::sqrt(var / dim + 1.0e-5);
        for (std::size_t d = 0; d < dim; ++d) values[row*dim+d]=(x.values()[row*dim+d]-mean)*inverse*w.weight[d]+w.bias[d];
    }
    return Tensor(x.dtype(), {shape.begin(),shape.end()}, std::move(values), std::string(x.device()));
}
Tensor gelu(Tensor x) {
    std::vector<double> values(x.values().begin(),x.values().end());
    for(auto& v:values)v*=0.5*(1.0+std::erf(v/std::sqrt(2.0)));
    return Tensor(x.dtype(),{x.shape().begin(),x.shape().end()},std::move(values),std::string(x.device()));
}
Tensor add(const Tensor& a,const Tensor& b){if(a.shape().size()!=b.shape().size()||!std::ranges::equal(a.shape(),b.shape()))throw std::invalid_argument("tensor add shape mismatch");std::vector<double> v(a.values().size());for(std::size_t i=0;i<v.size();++i)v[i]=a.values()[i]+b.values()[i];return Tensor(a.dtype(),{a.shape().begin(),a.shape().end()},std::move(v),std::string(a.device()));}

struct PatchData { std::size_t batch{},patches{},size{}; std::vector<std::int64_t> ids; std::vector<std::uint8_t> mask; };
PatchData patches(const MosaicTokenBatch& rows,const MosaicTextConfig& c){rectangular(rows,rows.size(),"text");const auto body=rows.front().size()-1,p=(body+c.patch_size-1)/c.patch_size;PatchData r{rows.size(),p,c.patch_size,std::vector<std::int64_t>(rows.size()*p*c.patch_size,mosaic_pad_id),std::vector<std::uint8_t>(rows.size()*p)};for(std::size_t b=0;b<rows.size();++b){for(std::size_t i=0;i<body;++i)r.ids[b*p*c.patch_size+i]=rows[b][i+1];for(std::size_t q=0;q<p;++q)for(std::size_t o=0;o<c.patch_size;++o)r.mask[b*p+q]|=r.ids[(b*p+q)*c.patch_size+o]!=mosaic_pad_id;}return r;}
BooleanMask mask_of(const PatchData& p,const bool bos){std::vector<std::uint8_t> v(p.batch*(p.patches+(bos?1:0)),bos?1:0);for(std::size_t b=0;b<p.batch;++b)std::copy_n(p.mask.begin()+static_cast<std::ptrdiff_t>(b*p.patches),p.patches,v.begin()+static_cast<std::ptrdiff_t>(b*(p.patches+(bos?1:0))+(bos?1:0)));return BooleanMask({p.batch,p.patches+(bos?1:0)},std::move(v));}
Tensor encoded_with_bos(const MosaicTextLM& model,const MosaicTokenBatch& rows){const auto& c=model.config();const auto& w=model.weights();const auto p=patches(rows,c);const auto flat=c.patch_size*c.byte_embedding_dim;std::vector<double> values(p.batch*(p.patches+1)*c.model_dim);for(std::size_t b=0;b<p.batch;++b){std::copy(w.bos_patch.begin(),w.bos_patch.end(),values.begin()+static_cast<std::ptrdiff_t>(b*(p.patches+1)*c.model_dim));for(std::size_t q=0;q<p.patches;++q){std::vector<double> raw(c.model_dim);for(std::size_t out=0;out<c.model_dim;++out){double x=w.patch_projection_bias[out];for(std::size_t o=0;o<c.patch_size;++o){const auto token=static_cast<std::size_t>(p.ids[(b*p.patches+q)*c.patch_size+o]);for(std::size_t d=0;d<c.byte_embedding_dim;++d)x+=w.byte_embedding[token*c.byte_embedding_dim+d]*w.patch_projection_weight[out*flat+o*c.byte_embedding_dim+d];}raw[out]=x;}double mean=std::accumulate(raw.begin(),raw.end(),0.0)/c.model_dim,var=0;for(const auto x:raw)var+=(x-mean)*(x-mean);const auto inv=1/std::sqrt(var/c.model_dim+1e-5);for(std::size_t d=0;d<c.model_dim;++d)values[(b*(p.patches+1)+q+1)*c.model_dim+d]=(raw[d]-mean)*inv*w.patch_norm_weight[d]+w.patch_norm_bias[d];}}return Tensor(TensorDType::float32,{p.batch,p.patches+1,c.model_dim},std::move(values),"cpu");}
Tensor masked_mean(const Tensor& x,const BooleanMask& m){const auto s=x.shape();const auto b=static_cast<std::size_t>(s[0]),t=static_cast<std::size_t>(s[1]),d=static_cast<std::size_t>(s[2]);std::vector<double> out(b*d);for(std::size_t r=0;r<b;++r){std::size_t n=0;for(std::size_t q=0;q<t;++q)if(m.at(r,q)){++n;for(std::size_t j=0;j<d;++j)out[r*d+j]+=x.values()[(r*t+q)*d+j];}for(std::size_t j=0;j<d;++j)out[r*d+j]/=std::max<std::size_t>(n,1);}return Tensor(x.dtype(),{b,d},std::move(out),std::string(x.device()));}
std::vector<double> softmax_row(std::span<const double> row){const auto maximum=*std::max_element(row.begin(),row.end());std::vector<double> p(row.size());double sum=0;for(std::size_t i=0;i<row.size();++i){p[i]=std::exp(row[i]-maximum);sum+=p[i];}for(auto&v:p)v/=sum;return p;}
double cross_entropy(const Tensor& logits,std::span<const std::int64_t> labels){const auto classes=static_cast<std::size_t>(logits.shape()[1]);double loss=0;for(std::size_t b=0;b<labels.size();++b){auto p=softmax_row(logits.values().subspan(b*classes,classes));loss-=std::log(p[static_cast<std::size_t>(labels[b])]);}return loss/labels.size();}
MosaicTextOutput replace_logits(const MosaicTextLM& model,const MosaicTokenBatch& inputs,const MosaicTokenBatch* targets,MosaicTextOutput base,Tensor logits){const auto s=logits.shape();const auto expected=static_cast<std::size_t>(s[1]*s[2]);MosaicTokenBatch raw;if(targets){raw=*targets;if(!raw.empty()&&raw.front().size()==inputs.front().size())for(auto&r:raw)r.erase(r.begin());}else{raw.resize(inputs.size());for(std::size_t b=0;b<inputs.size();++b)raw[b]={inputs[b].begin()+1,inputs[b].end()};}std::vector<std::uint8_t> mask(inputs.size()*expected);double loss=0;std::size_t n=0;for(std::size_t b=0;b<inputs.size();++b){if(raw[b].size()>expected)throw std::invalid_argument("targets are longer than the input token sequence");for(std::size_t i=0;i<raw[b].size();++i)if(raw[b][i]!=mosaic_pad_id){const auto label=raw[b][i];if(label<0||label>=mosaic_vocab_size)throw std::invalid_argument("target id is outside the byte vocabulary");mask[b*expected+i]=1;const auto begin=(b*expected+i)*vocab,maximum=*std::max_element(logits.values().begin()+static_cast<std::ptrdiff_t>(begin),logits.values().begin()+static_cast<std::ptrdiff_t>(begin+vocab));double denominator=0;for(std::size_t v=0;v<vocab;++v)denominator+=std::exp(logits.values()[begin+v]-maximum);loss+=std::log(denominator)+maximum-logits.values()[begin+static_cast<std::size_t>(label)];++n;}}base.logits=std::move(logits);base.loss=n?std::optional<double>(loss/n):std::nullopt;base.target_mask=std::move(mask);return base;}

} // namespace

void MosaicOmniTextPipelineConfig::validate(const MosaicTextConfig& text) const {static const std::set<std::string,std::less<>> modes{"pooled","token-cross","contextual-cross","consistency-cross","body-cross","core-body-cross","core-compact-body-cross","core-projected-compact-body-cross","core-lexical-compact-body-cross","core-lexical-consistency-cross"};if(!world_dim||!modes.contains(answerability_mode)||answerability_classes<2||answerability_classes>8||epistemic_supported_class>=answerability_classes||epistemic_memory_slots==0||!(epistemic_output_threshold>=0&&epistemic_output_threshold<=1))throw std::invalid_argument("invalid text pipeline configuration");text.validate();}

MosaicOmniTextPipeline::MosaicOmniTextPipeline(const MosaicTextLM& text,MosaicOmniTextPipelineConfig config,MosaicOmniTextPipelineWeights weights,MosaicOmniTextPipelineAdapters adapters):text_core_(&text),config_(std::move(config)),weights_(std::move(weights)),adapters_(adapters){config_.validate(text.config());const auto& c=text.config();linear_shape(weights_.text_to_world,c.model_dim,config_.world_dim);linear_shape(weights_.world_to_text_memory,config_.world_dim,c.retriever_dim);if(weights_.text_modality_embedding.size()!=config_.world_dim)throw std::invalid_argument("text modality embedding shape mismatch");if(weights_.text_only_to_world)linear_shape(*weights_.text_only_to_world,c.model_dim,config_.world_dim);if(weights_.text_only_world_to_text_memory)linear_shape(*weights_.text_only_world_to_text_memory,config_.world_dim,c.retriever_dim);if(weights_.logit_hidden&&weights_.logit_output){linear_shape(*weights_.logit_hidden,vocab,vocab*2);linear_shape(*weights_.logit_output,vocab*2,vocab);}else if(weights_.logit_hidden||weights_.logit_output)throw std::invalid_argument("logit adapter topology mismatch");if(config_.answerability_head){if(cross_modes.contains(config_.answerability_mode)){if(!adapters_.answerability)throw std::invalid_argument("cross answerability verifier is missing");}else{if(!weights_.pooled_answerability)throw std::invalid_argument("pooled answerability weights are missing");const auto& p=*weights_.pooled_answerability;norm_shape(p.norm,config_.world_dim*4+2);linear_shape(p.hidden,config_.world_dim*4+2,config_.world_dim);linear_shape(p.output,config_.world_dim,config_.answerability_classes);}}if(weights_.epistemic_memory)linear_shape(*weights_.epistemic_memory,config_.answerability_classes,c.retriever_dim,false);}

MosaicOmniTextSourceOutput MosaicOmniTextPipeline::prepare_text_source(
    const MosaicOmniTextSourceInputs& input) const {
    auto encoded = text_core_->encode_unified_source(input.world_input_ids);
    auto text_world = linear(
        encoded.states, weights_.text_to_world, config_.world_dim);
    if (input.text_only && weights_.text_only_to_world) {
        text_world = add(
            text_world,
            linear(encoded.states, *weights_.text_only_to_world,
                   config_.world_dim));
    }

    const auto patch_shape = encoded.patch_mask.shape();
    if (patch_shape.size() != 2) {
        throw std::invalid_argument("encoded text patch mask must have rank 2");
    }
    const auto batch = static_cast<std::size_t>(patch_shape[0]);
    const auto patches = static_cast<std::size_t>(patch_shape[1]);
    std::vector<std::uint8_t> mask_values(batch * (patches + 1));
    for (std::size_t row = 0; row < batch; ++row) {
        mask_values[row * (patches + 1)] = 1;
        std::copy_n(
            encoded.patch_mask.values().begin() +
                static_cast<std::ptrdiff_t>(row * patches),
            patches,
            mask_values.begin() +
                static_cast<std::ptrdiff_t>(row * (patches + 1) + 1));
    }
    BooleanMask source_mask(
        {static_cast<std::uint64_t>(batch),
         static_cast<std::uint64_t>(patches + 1)},
        std::move(mask_values));
    auto retrieval_summary = masked_mean(text_world, source_mask);

    const auto shape = text_world.shape();
    const auto rows = text_world.values().size() / config_.world_dim;
    std::vector<double> token_values(text_world.values().begin(),
                                     text_world.values().end());
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t dimension = 0; dimension < config_.world_dim;
             ++dimension) {
            token_values[row * config_.world_dim + dimension] +=
                weights_.text_modality_embedding[dimension];
        }
    }
    Tensor source_tokens(
        text_world.dtype(), {shape.begin(), shape.end()},
        std::move(token_values), std::string(text_world.device()));
    return {
        std::move(encoded.states),
        std::move(text_world),
        std::move(source_tokens),
        std::move(source_mask),
        std::move(retrieval_summary),
        "text",
    };
}

MosaicOmniTextPipelineOutput MosaicOmniTextPipeline::forward(const MosaicOmniTextPipelineInputs& in) const {
    const auto batch=in.input_ids.size();rectangular(in.input_ids,batch,"input_ids");const auto& world_ids=in.world_input_ids.empty()?in.input_ids:in.world_input_ids;rectangular(world_ids,batch,"world_input_ids");if(in.question_input_ids)rectangular(*in.question_input_ids,batch,"question_input_ids");
    const auto& tc=text_core_->config();const auto ws=in.world.shape(),ts=in.text_states.shape();
    if(ws.size()!=3||ws[0]!=batch||ws[2]!=config_.world_dim||ts.size()!=3||ts[0]!=batch||ts[2]!=tc.model_dim||in.text_mask.shape().size()!=2||in.text_mask.shape()[0]!=batch||in.text_mask.shape()[1]+1!=ts[1])throw std::invalid_argument("text pipeline upstream tensor shape mismatch");
    std::optional<Tensor> answer_logits;std::optional<double> answer_loss;std::optional<std::vector<std::uint8_t>> epistemic_active;
    const auto evidence_patches=patches(world_ids,tc);const auto evidence_full_mask=mask_of(evidence_patches,true);
    if(config_.answerability_head&&in.text_only){
        const auto& question_ids=in.question_input_ids?*in.question_input_ids:in.input_ids;const auto question_patches=patches(question_ids,tc);const auto question_patch_mask=mask_of(question_patches,false);const bool core=core_modes.contains(config_.answerability_mode);
        Tensor question_states=core?text_core_->forward(question_ids,nullptr,in.text_rounds).context_states:encoded_with_bos(*text_core_,question_ids);
        MosaicTokenBatch verifier_ids=compact_modes.contains(config_.answerability_mode)?compact_body_input_ids(world_ids):world_ids;
        const auto verifier_patches=patches(verifier_ids,tc);Tensor verifier_states=core?text_core_->forward(verifier_ids,nullptr,in.text_rounds).context_states:in.text_states.clone();
        BooleanMask verifier_patch_mask=core?mask_of(verifier_patches,false):in.text_mask;
        auto question_world=linear(question_states,weights_.text_to_world,config_.world_dim);auto evidence_world=linear(verifier_states,weights_.text_to_world,config_.world_dim);
        if(weights_.text_only_to_world){question_world=add(question_world,linear(question_states,*weights_.text_only_to_world,config_.world_dim));evidence_world=add(evidence_world,linear(verifier_states,*weights_.text_only_to_world,config_.world_dim));}
        const auto question_active=core?question_patch_mask:mask_of(question_patches,true);const auto evidence_active=core?verifier_patch_mask:evidence_full_mask;
        const auto question_summary=masked_mean(question_world,question_active),evidence_summary=masked_mean(evidence_world,evidence_active);
        if(cross_modes.contains(config_.answerability_mode)){
            const Tensor& verifier_question=projected_modes.contains(config_.answerability_mode)?question_states:question_world;
            const Tensor& verifier_evidence=projected_modes.contains(config_.answerability_mode)?verifier_states:evidence_world;
            std::optional<BooleanMask> body,title;std::optional<Tensor> extra;
            if(body_modes.contains(config_.answerability_mode)){
                if(compact_modes.contains(config_.answerability_mode))body=verifier_patch_mask;
                else{
                    std::vector<std::uint8_t> values(batch*(evidence_patches.patches+(core?0:1)));for(std::size_t b=0;b<batch;++b){bool after=false;for(std::size_t p=0;p<evidence_patches.patches;++p){bool newline=false;for(std::size_t o=0;o<tc.patch_size;++o)newline|=evidence_patches.ids[(b*evidence_patches.patches+p)*tc.patch_size+o]==static_cast<std::int64_t>('\n');after|=newline;values[b*(evidence_patches.patches+(core?0:1))+(core?0:1)+p]=after&&evidence_patches.mask[b*evidence_patches.patches+p];}}
                    body=BooleanMask({batch,evidence_patches.patches+(core?0:1)},std::move(values));
                }
                std::vector<std::uint8_t> title_values(evidence_active.values().size());for(std::size_t i=0;i<title_values.size();++i)title_values[i]=evidence_active.values()[i]&&!body->values()[i];for(std::size_t b=0;b<batch;++b)title_values[b*evidence_active.shape()[1]]=0;title=BooleanMask({evidence_active.shape().begin(),evidence_active.shape().end()},std::move(title_values));
            }
            if(lexical_modes.contains(config_.answerability_mode)){const auto lexical_evidence=config_.answerability_mode=="core-lexical-consistency-cross"?compact_body_input_ids(world_ids):verifier_ids;extra=byte_ngram_overlap_features(question_ids,lexical_evidence);}
            answer_logits=adapters_.answerability->forward(verifier_question,question_active,verifier_evidence,evidence_active,title?&*title:nullptr,body?&*body:nullptr,extra?&*extra:nullptr);
        }else{
            const auto dim=config_.world_dim;std::vector<double> f(batch*(dim*4+2));for(std::size_t b=0;b<batch;++b){for(std::size_t d=0;d<dim;++d){const auto q=question_summary.values()[b*dim+d],e=evidence_summary.values()[b*dim+d];f[b*(dim*4+2)+d]=q;f[b*(dim*4+2)+dim+d]=e;f[b*(dim*4+2)+2*dim+d]=std::abs(q-e);f[b*(dim*4+2)+3*dim+d]=q*e;}double qd=0,ed=0;for(std::size_t t=0;t<question_active.shape()[1];++t)qd+=question_active.at(b,t);for(std::size_t t=0;t<evidence_active.shape()[1];++t)ed+=evidence_active.at(b,t);f[b*(dim*4+2)+4*dim]=qd/question_active.shape()[1];f[b*(dim*4+2)+4*dim+1]=ed/evidence_active.shape()[1];}
            const auto& p=*weights_.pooled_answerability;answer_logits=linear(gelu(linear(norm(Tensor(TensorDType::float32,{batch,dim*4+2},std::move(f),"cpu"),p.norm),p.hidden,dim)),p.output,config_.answerability_classes);
        }
        if(in.answerability_labels){if(in.answerability_labels->size()!=batch||std::ranges::any_of(*in.answerability_labels,[&](auto v){return v<0||static_cast<std::size_t>(v)>=config_.answerability_classes;}))throw std::invalid_argument("answerability_labels must be [batch] class indices");answer_loss=cross_entropy(*answer_logits,*in.answerability_labels);epistemic_active.emplace(batch);for(std::size_t b=0;b<batch;++b)(*epistemic_active)[b]=static_cast<std::size_t>((*in.answerability_labels)[b])!=config_.epistemic_supported_class;}else{epistemic_active.emplace(batch);for(std::size_t b=0;b<batch;++b){const auto p=softmax_row(answer_logits->values().subspan(b*config_.answerability_classes,config_.answerability_classes));(*epistemic_active)[b]=(1.0-p[config_.epistemic_supported_class])>=config_.epistemic_output_threshold;}}
    }
    auto text_memory=linear(in.world,weights_.world_to_text_memory,tc.retriever_dim);if(in.text_only&&weights_.text_only_world_to_text_memory)text_memory=add(text_memory,linear(in.world,*weights_.text_only_world_to_text_memory,tc.retriever_dim));
    if(in.text_only&&weights_.epistemic_memory){if(!answer_logits)throw std::runtime_error("epistemic logits are unavailable");std::vector<double> distributions(batch*config_.answerability_classes);for(std::size_t b=0;b<batch;++b){const auto p=softmax_row(answer_logits->values().subspan(b*config_.answerability_classes,config_.answerability_classes));std::copy(p.begin(),p.end(),distributions.begin()+static_cast<std::ptrdiff_t>(b*config_.answerability_classes));}const auto memory=linear(Tensor(TensorDType::float32,{batch,config_.answerability_classes},std::move(distributions),"cpu"),*weights_.epistemic_memory,tc.retriever_dim);auto values=std::vector<double>(text_memory.values().begin(),text_memory.values().end());const auto slots=static_cast<std::size_t>(ws[1]);if(config_.epistemic_memory_slots>slots)throw std::invalid_argument("epistemic memory slots must fit the world workspace");for(std::size_t b=0;b<batch;++b)for(std::size_t slot=slots-config_.epistemic_memory_slots;slot<slots;++slot)for(std::size_t d=0;d<tc.retriever_dim;++d)values[(b*slots+slot)*tc.retriever_dim+d]+=memory.values()[b*tc.retriever_dim+d];text_memory=Tensor(text_memory.dtype(),{text_memory.shape().begin(),text_memory.shape().end()},std::move(values),std::string(text_memory.device()));}
    auto text=text_core_->forward(in.input_ids,in.targets?&*in.targets:nullptr,in.text_rounds,nullptr,&text_memory,nullptr);
    if(in.text_only&&(weights_.logit_hidden||adapters_.cross_memory||adapters_.hidden_cross_memory||adapters_.epistemic_output)){auto logits=text.logits.clone();if(weights_.logit_hidden)logits=add(logits,linear(gelu(linear(text.logits,*weights_.logit_hidden,vocab*2)),*weights_.logit_output,vocab));if(adapters_.cross_memory)logits=add(logits,adapters_.cross_memory->forward(text.logits,in.text_states,evidence_full_mask));if(adapters_.hidden_cross_memory)logits=add(logits,adapters_.hidden_cross_memory->forward(text.decoder_states,in.text_states,evidence_full_mask));if(adapters_.epistemic_output){if(!epistemic_active)throw std::runtime_error("epistemic decoder state is unavailable");logits=add(logits,adapters_.epistemic_output->forward(text.decoder_states,*epistemic_active));}text=replace_logits(*text_core_,in.input_ids,in.targets?&*in.targets:nullptr,std::move(text),std::move(logits));}
    return {std::move(text),std::move(answer_logits),answer_loss,std::move(epistemic_active),std::move(text_memory)};
}

} // namespace swegca::world
