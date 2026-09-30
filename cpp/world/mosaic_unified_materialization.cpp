#include "world/mosaic_unified_materialization.hpp"

#include "checkpoint/materialized_tensor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

namespace swegca::world {
namespace {
class Reader final {
public:
    explicit Reader(const MosaicStateDict& state) : state_(state) {}
    std::vector<double> d(std::string key,std::initializer_list<std::uint64_t> shape){const auto&t=get(key,shape);std::vector<double>v(t.values().begin(),t.values().end());return v;}
    std::vector<float> f(std::string key,std::initializer_list<std::uint64_t> shape){const auto&t=get(key,shape);std::vector<float>v;v.reserve(t.values().size());for(auto x:t.values())v.push_back(static_cast<float>(x));return v;}
    double scalar(std::string key){const auto&t=get(key,{});return t.values().front();}
    checkpoint::MaterializedTensor m(std::string key,std::initializer_list<std::uint64_t> shape){auto v=f(key,shape);std::vector<std::byte>b(v.size()*sizeof(float));std::memcpy(b.data(),v.data(),b.size());return checkpoint::MaterializedTensor(checkpoint::TensorDType::float32,{shape.begin(),shape.end()},std::move(b));}
    void finish(std::initializer_list<std::string_view> prefixes)const{for(const auto&[key,value]:state_){static_cast<void>(value);if(used_.contains(key))continue;for(auto p:prefixes)if(key.starts_with(p))throw std::invalid_argument("unexpected or disabled state_dict key: "+key);}}
private:
    const Tensor&get(const std::string&key,std::initializer_list<std::uint64_t>shape){auto it=state_.find(key);if(it==state_.end())throw std::invalid_argument("missing state_dict key: "+key);const auto&t=it->second;if(t.device()!="cpu")throw std::invalid_argument("state_dict tensor must be CPU: "+key);if(t.shape().size()!=shape.size()||!std::equal(t.shape().begin(),t.shape().end(),shape.begin()))throw std::invalid_argument("state_dict shape mismatch: "+key);if(!std::ranges::all_of(t.values(),[](double x){return std::isfinite(x);}))throw std::invalid_argument("state_dict tensor is non-finite: "+key);used_.insert(key);return t;}
    const MosaicStateDict&state_;std::set<std::string,std::less<>>used_;
};
ModalLinearWeights ml(Reader&r,const std::string&p,std::size_t o,std::size_t i,bool b=true){return{r.d(p+".weight",{o,i}),b?r.d(p+".bias",{o}):std::vector<double>{}};}
ModalConvWeights mc2(Reader&r,const std::string&p,std::size_t o,std::size_t i,std::size_t k,bool b=true){return{r.d(p+".weight",{o,i,k,k}),b?r.d(p+".bias",{o}):std::vector<double>{}};}
ModalConvWeights mc1(Reader&r,const std::string&p,std::size_t o,std::size_t i,std::size_t k){return{r.d(p+".weight",{o,i,k}),r.d(p+".bias",{o})};}
ModalGruWeights mg(Reader&r,const std::string&p,std::size_t d){return{r.d(p+".weight_ih_l0",{3*d,d}),r.d(p+".weight_hh_l0",{3*d,d}),r.d(p+".bias_ih_l0",{3*d}),r.d(p+".bias_hh_l0",{3*d})};}
TextAdapterLinearWeights tl(Reader&r,const std::string&p,std::size_t o,std::size_t i,bool b=true){return{r.d(p+".weight",{o,i}),b?r.d(p+".bias",{o}):std::vector<double>{}};}
TextAdapterNormWeights tn(Reader&r,const std::string&p,std::size_t d){return{r.d(p+".weight",{d}),r.d(p+".bias",{d})};}
TextAdapterAttentionWeights ta(Reader&r,const std::string&p,std::size_t d){return{{r.d(p+".in_proj_weight",{3*d,d}),r.d(p+".in_proj_bias",{3*d})},tl(r,p+".out_proj",d,d)};}
WorldPipelineLinear wl(Reader&r,const std::string&p,std::size_t o,std::size_t i,bool b=true){return{r.d(p+".weight",{o,i}),b?r.d(p+".bias",{o}):std::vector<double>{}};}
}

void require_disabled_state_dict_prefix(const MosaicStateDict&s,std::string_view p){for(const auto&[k,v]:s){static_cast<void>(v);if(k.starts_with(p))throw std::invalid_argument("disabled module key is present: "+k);}}

MosaicOmniModalFrontendWeights materialize_modal_frontends(const MosaicStateDict&s,const MosaicOmniModalFrontendConfig&c){Reader r(s);auto D=c.world_dim,K=c.vision_patch_size;MosaicOmniModalFrontendWeights w;w.vision_frontend=mc2(r,"vision_frontend",D,3,K);if(c.visual_semantic_encoder&&c.visual_semantic_split_frontend)w.visual_semantic_frontend=mc2(r,"visual_semantic_frontend",D,3,K);if(c.visual_semantic_encoder){w.visual_position=ml(r,"visual_position",D,2,false);VisualSemanticCellWeights x;x.attention_in_weight=r.d("visual_semantic_cell.self_attn.in_proj_weight",{3*D,D});x.attention_in_bias=r.d("visual_semantic_cell.self_attn.in_proj_bias",{3*D});x.attention_out_weight=r.d("visual_semantic_cell.self_attn.out_proj.weight",{D,D});x.attention_out_bias=r.d("visual_semantic_cell.self_attn.out_proj.bias",{D});x.linear1_weight=r.d("visual_semantic_cell.linear1.weight",{c.world_ffn_dim,D});x.linear1_bias=r.d("visual_semantic_cell.linear1.bias",{c.world_ffn_dim});x.linear2_weight=r.d("visual_semantic_cell.linear2.weight",{D,c.world_ffn_dim});x.linear2_bias=r.d("visual_semantic_cell.linear2.bias",{D});x.norm1_weight=r.d("visual_semantic_cell.norm1.weight",{D});x.norm1_bias=r.d("visual_semantic_cell.norm1.bias",{D});x.norm2_weight=r.d("visual_semantic_cell.norm2.weight",{D});x.norm2_bias=r.d("visual_semantic_cell.norm2.bias",{D});w.visual_semantic_cell=std::move(x);w.visual_semantic_norm=ModalLinearWeights{r.d("visual_semantic_norm.weight",{D}),r.d("visual_semantic_norm.bias",{D})};}if(c.image_visual_adapter){w.image_visual_adapter_down=mc2(r,"image_visual_adapter_down",c.image_visual_adapter_rank,3,K,false);w.image_visual_adapter_up=mc2(r,"image_visual_adapter_up",D,c.image_visual_adapter_rank,1,false);}w.audio_frontend=mc1(r,"audio_frontend",D,1,c.audio_patch_samples);if(c.audio_temporal_encoder){w.audio_temporal_cell=mg(r,"audio_temporal_cell",D);w.audio_temporal_to_world=ml(r,"audio_temporal_to_world",D,D);}if(c.audio_content_encoder){w.audio_content_temporal_cell=mg(r,"audio_content_temporal_cell",D);w.audio_content_to_world=ml(r,"audio_content_to_world",D,D);}if(c.audio_spectral_content_frontend)w.audio_spectral_projection=ml(r,"audio_spectral_projection",D,c.audio_spectral_n_fft/2+1);if(c.audio_event_slot_injection)w.audio_event_slot_projection=ml(r,"audio_event_slot_projection",D,D);if(c.audio_ctc_head)w.audio_ctc_projection=ml(r,"audio_ctc_projection",259,D);if(c.audio_grapheme_ctc_vocabulary_size)w.audio_grapheme_ctc_projection=ml(r,"audio_grapheme_ctc_projection",c.audio_grapheme_ctc_vocabulary_size+1,D);if(c.audio_text_retrieval_head){w.audio_text_retrieval_projection=ml(r,"audio_text_retrieval_projection",D,D,false);w.text_audio_retrieval_projection=ml(r,"text_audio_retrieval_projection",D,D,false);}if(c.audio_teacher_dim)w.audio_teacher_projection=ml(r,"audio_teacher_projection",c.audio_teacher_dim,D);if(c.audio_temporal_binary_head){w.audio_temporal_head_norm=ModalLinearWeights{r.d("audio_temporal_head.0.weight",{D}),r.d("audio_temporal_head.0.bias",{D})};w.audio_temporal_head_output=ml(r,"audio_temporal_head.1",2,D);}auto modality=r.d("modality_embedding.weight",{4,D});w.image_modality_embedding.assign(modality.begin()+D,modality.begin()+2*D);w.audio_modality_embedding.assign(modality.begin()+2*D,modality.begin()+3*D);r.finish({"vision_frontend.","visual_semantic_frontend.","visual_position.","visual_semantic_cell.","visual_semantic_norm.","image_visual_adapter_","audio_frontend.","audio_temporal_","audio_content_","audio_spectral_","audio_event_","audio_ctc_","audio_grapheme_","audio_text_","text_audio_","audio_teacher_","modality_embedding."});w.validate(c);return w;}

ModalToWorldWeights materialize_modal_to_world(const MosaicStateDict&s,const ModalToWorldConfig&c,std::string_view prefix){Reader r(s);std::string p(prefix);auto D=c.world.world_dim,S=c.world.world_slots,I=c.source_dim;ModalToWorldWeights w(r.m(p+".world_queries",{S,D}),r.m(p+".source_projection.0.weight",{I}),r.m(p+".source_projection.0.bias",{I}),r.m(p+".source_projection.1.weight",{D,I}),r.m(p+".source_projection.1.bias",{D}),r.m(p+".cross_attention.in_proj_weight",{3*D,D}),r.m(p+".cross_attention.in_proj_bias",{3*D}),r.m(p+".cross_attention.out_proj.weight",{D,D}),r.m(p+".cross_attention.out_proj.bias",{D}),r.m(p+".output_norm.weight",{D}),r.m(p+".output_norm.bias",{D}),c);r.finish({prefix});return w;}

VisualTeacherSlotBridgeWeights materialize_visual_teacher_slot_bridge(const MosaicStateDict&s,std::size_t D,std::size_t R){Reader r(s);std::string p="visual_teacher_slot_bridge.";VisualTeacherSlotBridgeWeights w{r.d(p+"slot_norm.weight",{D}),r.d(p+"slot_norm.bias",{D}),r.d(p+"patch_norm.weight",{D}),r.d(p+"patch_norm.bias",{D}),r.d(p+"query.weight",{R,D}),r.d(p+"key.weight",{R,D}),r.d(p+"value_down.weight",{R,D}),r.d(p+"value_down.bias",{R}),r.d(p+"value_up.weight",{D,R}),r.d(p+"value_up.bias",{D})};r.finish({p});w.validate(D,R);return w;}
ExplicitObjectRelationGrounderWeights materialize_explicit_grounder(const MosaicStateDict&s,std::size_t D,std::size_t R){Reader r(s);std::string p="explicit_object_relation_grounder.";auto Q=std::min(D,2*R);ExplicitObjectRelationGrounderWeights w{r.d(p+"descriptor_norm.weight",{D}),r.d(p+"descriptor_norm.bias",{D}),r.d(p+"patch_norm.weight",{D}),r.d(p+"patch_norm.bias",{D}),r.d(p+"query.weight",{R,D}),r.d(p+"key.weight",{R,D}),r.d(p+"value.weight",{R,D}),r.d(p+"value.bias",{R}),r.d(p+"object_up.weight",{D,R+4}),r.d(p+"object_up.bias",{D}),r.d(p+"relation_norm.weight",{4*D}),r.d(p+"relation_norm.bias",{4*D}),r.d(p+"relation_down.weight",{Q,4*D}),r.d(p+"relation_down.bias",{Q}),r.d(p+"relation_up.weight",{D,Q}),r.d(p+"relation_up.bias",{D})};r.finish({p});w.validate(D,R);return w;}
ExplicitRelationHeadWeights materialize_explicit_relation_head(const MosaicStateDict&s,std::size_t D,std::size_t C){Reader r(s);std::string p="explicit_relation_head.";ExplicitRelationHeadWeights w{r.d(p+"norm.weight",{D}),r.d(p+"norm.bias",{D}),r.d(p+"output.weight",{C,D}),r.d(p+"output.bias",{C})};r.finish({p});w.validate(D,C);return w;}

MosaicOmniWorldPipelineWeights materialize_world_pipeline(
    const MosaicStateDict& s, const MosaicUnifiedConfig& c,
    const MosaicOmniWorldPipelineConfig& pc) {
    Reader r(s); const auto d=c.omni.world_dim;
    MosaicOmniWorldPipelineWeights w;
    if(c.video_object_temporal_encoder)w.video_object_to_world=wl(r,"video_object_to_world",d,d);
    w.video_temporal_to_world=wl(r,"video_temporal_to_world",d,d);
    if(c.video_separate_temporal_delta_projection)w.video_temporal_delta_to_world=wl(r,"video_temporal_delta_to_world",d,d,false);
    if(c.audio_event_slot_injection)w.audio_event_slot_projection=wl(r,"audio_event_slot_projection",d,d);
    if(c.audio_content_encoder)w.audio_content_to_world=wl(r,"audio_content_to_world",d,d);
    auto& x=w.world_cell;
    x.attention_in_weight=r.d("world_cell.self_attn.in_proj_weight",{3*d,d});
    x.attention_in_bias=r.d("world_cell.self_attn.in_proj_bias",{3*d});
    x.attention_out_weight=r.d("world_cell.self_attn.out_proj.weight",{d,d});
    x.attention_out_bias=r.d("world_cell.self_attn.out_proj.bias",{d});
    x.feedforward_in=wl(r,"world_cell.linear1",pc.world_ffn_dim,d);
    x.feedforward_out=wl(r,"world_cell.linear2",d,pc.world_ffn_dim);
    x.norm1_weight=r.d("world_cell.norm1.weight",{d});x.norm1_bias=r.d("world_cell.norm1.bias",{d});
    x.norm2_weight=r.d("world_cell.norm2.weight",{d});x.norm2_bias=r.d("world_cell.norm2.bias",{d});
    r.finish({"video_object_to_world.","video_temporal_to_world.","video_temporal_delta_to_world.","audio_event_slot_projection.","audio_content_to_world.","world_cell."});
    pc.validate(); return w;
}

CrossModalEvidenceWeights materialize_cross_modal_evidence(
    const MosaicStateDict& s, const MosaicUnifiedConfig& c) {
    if(!c.cross_modal_evidence_head)throw std::invalid_argument("cross-modal evidence module is disabled");
    Reader r(s);const auto d=c.omni.world_dim,q=static_cast<std::size_t>(c.cross_modal_evidence_rank);
    const auto n=q*(c.cross_modal_evidence_direct_features?6U:3U);CrossModalEvidenceWeights w;
    w.text_projection=r.f("cross_modal_text_projection.weight",{q,d});
    w.audio_projection=r.f("cross_modal_audio_projection.weight",{q,d});
    w.video_projection=r.f("cross_modal_video_projection.weight",{q,d});
    if(c.cross_modal_text_query_pooling)w.text_query=CrossModalQuerySummaryWeights{r.f("cross_modal_text_evidence_score.weight",{1,q}),0.0F};
    if(c.cross_modal_text_sequence_pooling)w.text_sequence=CrossModalSequenceSummaryWeights{
        r.f("cross_modal_text_sequence_encoder.weight_ih_l0",{3*q,q}),
        r.f("cross_modal_text_sequence_encoder.weight_hh_l0",{3*q,q}),
        r.f("cross_modal_text_sequence_encoder.bias_ih_l0",{3*q}),
        r.f("cross_modal_text_sequence_encoder.bias_hh_l0",{3*q})};
    w.normalization_mean=r.f("cross_modal_evidence_norm.running_mean",{n});
    w.normalization_variance=r.f("cross_modal_evidence_norm.running_var",{n});
    if(r.scalar("cross_modal_evidence_norm.num_batches_tracked")<0.0)
        throw std::invalid_argument("negative cross-modal BatchNorm batch count");
    w.to_world_weight=r.f("cross_modal_evidence_to_world.weight",{d,n});
    w.to_world_bias=r.f("cross_modal_evidence_to_world.bias",{d});
    w.head_norm_weight=r.f("cross_modal_evidence_head.0.weight",{d});
    w.head_norm_bias=r.f("cross_modal_evidence_head.0.bias",{d});
    w.head_weight=r.f("cross_modal_evidence_head.1.weight",{2,d});
    w.head_bias=r.f("cross_modal_evidence_head.1.bias",{2});
    r.finish({"cross_modal_"});w.validate(c);return w;
}

MosaicOmniTextPipelineWeights materialize_text_pipeline(
    const MosaicStateDict&s,const MosaicUnifiedConfig&c,const MosaicOmniTextPipelineConfig&pc){
    Reader r(s);const auto d=c.omni.world_dim,md=c.text.model_dim,rd=c.text.retriever_dim,v=static_cast<std::size_t>(mosaic_vocab_size);
    MosaicOmniTextPipelineWeights w;
    w.text_to_world=tl(r,"text_to_world",d,md);w.world_to_text_memory=tl(r,"world_to_text_memory",rd,d);
    auto modality=r.d("modality_embedding.weight",{4,d});w.text_modality_embedding.assign(modality.begin(),modality.begin()+d);
    if(c.text_only_bridge_adapter){w.text_only_to_world=tl(r,"text_only_to_world",d,md);w.text_only_world_to_text_memory=tl(r,"text_only_world_to_text_memory",rd,d);}
    if(c.text_only_output_adapter){w.logit_hidden=tl(r,"text_only_logit_adapter.0",2*v,v);w.logit_output=tl(r,"text_only_logit_adapter.2",v,2*v);}
    if(c.text_answerability_head&&c.text_answerability_mode=="pooled"){
        const auto in=4*d+2;w.pooled_answerability=PooledAnswerabilityWeights{
            tn(r,"text_answerability_head.0",in),tl(r,"text_answerability_head.1",d,in),tl(r,"text_answerability_head.3",static_cast<std::size_t>(c.text_answerability_classes),d)};
    }
    if(c.text_epistemic_memory_adapter)w.epistemic_memory=tl(r,"text_epistemic_memory",rd,static_cast<std::size_t>(c.text_answerability_classes),false);
    r.finish({"text_to_world.","world_to_text_memory.","text_only_to_world.","text_only_world_to_text_memory.","text_only_logit_adapter.","text_epistemic_memory.","modality_embedding."});
    if(c.text_answerability_head&&c.text_answerability_mode=="pooled")r.finish({"text_answerability_head."});
    pc.validate(c.text);return w;
}

TextOnlyCrossMemoryWeights materialize_text_cross_memory(const MosaicStateDict&s,std::size_t d){Reader r(s);std::string p="text_only_cross_memory.";TextOnlyCrossMemoryWeights w{tl(r,p+"query",d,259),ta(r,p+"cross_attention",d),tl(r,p+"output",259,d)};r.finish({p});return w;}
TextOnlyHiddenCrossMemoryWeights materialize_text_hidden_cross_memory(const MosaicStateDict&s,std::size_t d){Reader r(s);std::string p="text_only_hidden_cross_memory.";TextOnlyHiddenCrossMemoryWeights w{tn(r,p+"query.0",d),tl(r,p+"query.1",d,d),ta(r,p+"cross_attention",d),tl(r,p+"output",259,d)};r.finish({p});return w;}
TextEpistemicOutputWeights materialize_text_epistemic_output(const MosaicStateDict&s,std::size_t d,std::size_t rank){Reader r(s);std::string p="text_epistemic_output.";TextEpistemicOutputWeights w{tl(r,p+"down",rank,d,false),tl(r,p+"output",259,rank)};r.finish({p});return w;}

TextAnswerabilityWeights materialize_text_answerability(const MosaicStateDict&s,const TextAnswerabilityConfig&c){
    Reader r(s);std::string p="text_answerability_head.";const auto d=c.world_dim;
    TextAnswerabilityWeights w;
    if(c.source_dim!=d){w.input_norm=tn(r,p+"input_projection.0",c.source_dim);w.input_projection=tl(r,p+"input_projection.1",d,c.source_dim);}
    if(c.contextual){TextAdapterEncoderWeights e;e.attention=ta(r,p+"context_encoder.self_attn",d);e.feedforward_in=tl(r,p+"context_encoder.linear1",2*d,d);e.feedforward_out=tl(r,p+"context_encoder.linear2",d,2*d);e.norm1=tn(r,p+"context_encoder.norm1",d);e.norm2=tn(r,p+"context_encoder.norm2",d);w.context_encoder=std::move(e);}
    w.question_norm=tn(r,p+"question_norm",d);w.evidence_norm=tn(r,p+"evidence_norm",d);w.cross_attention=ta(r,p+"cross_attention",d);
    w.token_norm=tn(r,p+"token_projection.0",4*d);w.token_projection=tl(r,p+"token_projection.1",d,4*d);
    const auto out=d*(c.evidence_consistency?4U:2U)+(c.evidence_consistency?3U:2U)+c.extra_feature_dim;
    w.output_norm=tn(r,p+"output.0",out);w.output_hidden=tl(r,p+"output.1",d,out);w.output=tl(r,p+"output.3",c.output_classes,d);
    r.finish({p});c.validate();return w;
}

NarrativeContinuityWeights materialize_narrative_weights(const MosaicStateDict&s,std::size_t td,std::size_t h,std::size_t wd){Reader r(s);NarrativeContinuityWeights w{h,wd,r.f("narrative_continuity_head.0.weight",{h,8*td}),r.f("narrative_continuity_head.0.bias",{h}),r.f("narrative_continuity_head.2.weight",{1,h}),r.f("narrative_continuity_head.2.bias",{1}),r.f("narrative_evidence_to_world.weight",{wd,1})};r.finish({"narrative_continuity_head.","narrative_evidence_to_world."});w.validate(td);return w;}

LongVideoWorldWeights materialize_long_video_weights(const MosaicStateDict&s,const MosaicOmniConfig&c,bool transition){Reader r(s);const auto d=c.world_dim;std::string p="long_video_accumulator.";LongVideoWorldWeights w;w.position_weight=r.f(p+"position.weight",{d,2});if(transition){w.transition_weight=r.f(p+"transition.weight",{d,2*d});w.transition_bias=r.f(p+"transition.bias",{d});}w.gru_weight_ih=r.f(p+"cell.weight_ih",{3*d,d});w.gru_weight_hh=r.f(p+"cell.weight_hh",{3*d,d});w.gru_bias_ih=r.f(p+"cell.bias_ih",{3*d});w.gru_bias_hh=r.f(p+"cell.bias_hh",{3*d});w.norm_weight=r.f(p+"norm.weight",{d});w.norm_bias=r.f(p+"norm.bias",{d});w.order_norm_weight=r.f(p+"order_head.0.weight",{d});w.order_norm_bias=r.f(p+"order_head.0.bias",{d});w.order_weight=r.f(p+"order_head.1.weight",{2,d});w.order_bias=r.f(p+"order_head.1.bias",{2});r.finish({p});w.validate(c,transition);return w;}

WorldToAnimaWeights materialize_world_to_anima_weights(const MosaicStateDict&s,const MosaicOmniConfig&c,std::string_view prefix){Reader r(s);std::string p(prefix);if(!p.empty())p+='.';const auto d=c.world_dim,q=c.anima_conditioning_tokens,a=c.anima_conditioning_dim;WorldToAnimaWeights w{r.f(p+"conditioning_queries",{q,d}),r.f(p+"cross_attention.in_proj_weight",{3*d,d}),r.f(p+"cross_attention.in_proj_bias",{3*d}),r.f(p+"cross_attention.out_proj.weight",{d,d}),r.f(p+"cross_attention.out_proj.bias",{d}),r.f(p+"output.0.weight",{d}),r.f(p+"output.0.bias",{d}),r.f(p+"output.1.weight",{a,d})};r.finish({p});w.validate(c);return w;}

VideoPipelineMaterializedWeights materialize_video_pipeline(
    const MosaicStateDict&s,const MosaicUnifiedConfig&c){
    Reader r(s);const auto cfg=VideoPipelineConfig::from_unified(c);const auto d=cfg.world_dim;
    VideoPipelineMaterializedWeights all;auto&w=all.pipeline;
    w.video_time=ml(r,"video_time",d,1,false);
    w.temporal_mixer_weight=r.d("video_temporal_mixer.weight",{d,1,3,1,1});
    w.temporal_cell=mg(r,"video_temporal_cell",d);w.temporal_to_world=ml(r,"video_temporal_to_world",d,d);
    if(cfg.separate_temporal_delta_projection)w.temporal_delta_to_world=ml(r,"video_temporal_delta_to_world",d,d,false);
    if(cfg.object_temporal_encoder){
        w.object_temporal_cell=mg(r,"video_object_temporal_cell",d);
        const auto k=std::max<std::size_t>(2,cfg.vision_patch_size/2);
        w.object_frontend=mc2(r,"video_object_frontend",d,3,k);
        w.object_to_world=ml(r,"video_object_to_world",d,d);w.object_statistics=ml(r,"video_object_statistics",d,3*d);
        const auto n=d*(cfg.object_slots+1);w.object_decision=VideoNormLinear{r.d("video_object_decision.0.weight",{n}),r.d("video_object_decision.0.bias",{n}),ml(r,"video_object_decision.1",d,n)};
    }
    if(cfg.object_camera_invariant_residual)w.object_camera_invariant_frontend=mc2(r,"video_object_camera_invariant_frontend",d,3,std::max<std::size_t>(2,cfg.vision_patch_size/2));
    if(cfg.object_set_decision)w.object_set_decision=VideoNormLinear{r.d("video_object_set_decision.0.weight",{2*d}),r.d("video_object_set_decision.0.bias",{2*d}),ml(r,"video_object_set_decision.1",d,2*d)};
    if(cfg.object_identity_event_binding)w.object_binding_decision=VideoNormLinear{r.d("video_object_binding_decision.0.weight",{4*d}),r.d("video_object_binding_decision.0.bias",{4*d}),ml(r,"video_object_binding_decision.1",d,4*d)};
    if(cfg.spatial_temporal_moment)w.spatial_moment_to_world=ml(r,"video_spatial_temporal_moment_to_world",d,2,false);
    if(cfg.query_spatial_temporal_moment)w.spatial_moment_gate=ml(r,"video_spatial_temporal_moment_gate",2,d);
    if(cfg.spatial_temporal_y_moment){w.spatial_y_moment_to_world=ml(r,"video_spatial_temporal_y_moment_to_world",d,2,false);w.spatial_y_moment_gate=ml(r,"video_spatial_temporal_y_moment_gate",2,d);}
    if(cfg.spatial_temporal_logit_head)w.spatial_logit_head=ml(r,"video_spatial_temporal_logit_head",2,d+4);
    if(cfg.spatial_temporal_bilinear_head)w.spatial_bilinear_head=ml(r,"video_spatial_temporal_bilinear_head",4,d);
    if(cfg.query_conditioned_head)w.query_conditioning=ml(r,"video_query_conditioning",2*d,d);
    if(cfg.object_dual_evidence)w.object_evidence_gate=ml(r,"video_object_evidence_gate",2,d);
    auto modality=r.d("modality_embedding.weight",{4,d});w.video_modality_embedding.assign(modality.begin()+3*d,modality.end());
    w.order_head=VideoNormLinear{r.d("video_order_head.0.weight",{d}),r.d("video_order_head.0.bias",{d}),ml(r,"video_order_head.1",2,d)};
    if(cfg.camera_robustness_adapter){
        if(cfg.camera_robustness_nonlinear_gate){w.camera_robustness_gate_nonlinear=VideoMlp{ml(r,"video_camera_robustness_gate.0",16,8),ml(r,"video_camera_robustness_gate.2",1,16)};}
        else w.camera_robustness_gate_linear=ml(r,"video_camera_robustness_gate",1,8);
        w.camera_robustness_head=ml(r,"video_camera_robustness_head",2,d,false);
    }
    if(cfg.camera_pose_dim)w.camera_pose_encoder=ml(r,"video_camera_pose_encoder",d,cfg.camera_pose_dim);
    if(cfg.spatial_relation_classes){const auto n=2*d+48+(cfg.camera_pose_dim?VideoSpatialGeometryReasoner::output_dim:0);w.spatial_relation_head=VideoNormMlp{r.d("video_spatial_relation_head.0.weight",{n}),r.d("video_spatial_relation_head.0.bias",{n}),ml(r,"video_spatial_relation_head.1",d,n),ml(r,"video_spatial_relation_head.3",cfg.spatial_relation_classes,d)};}
    if(cfg.action_dim)w.action_encoder=ml(r,"video_action_encoder",d,cfg.action_dim);
    if(cfg.egomotion_classes)w.egomotion_head=VideoNormMlp{r.d("video_egomotion_head.0.weight",{3*d}),r.d("video_egomotion_head.0.bias",{3*d}),ml(r,"video_egomotion_head.1",d,3*d),ml(r,"video_egomotion_head.3",cfg.egomotion_classes,d)};
    if(cfg.egomotion_validity_head)w.egomotion_validity_head_weights=VideoNormMlp{r.d("video_egomotion_validity_head.0.weight",{d+2}),r.d("video_egomotion_validity_head.0.bias",{d+2}),ml(r,"video_egomotion_validity_head.1",64,d+2),ml(r,"video_egomotion_validity_head.3",2,64)};
    if(cfg.vision_teacher_dim)w.teacher_projection=ml(r,"video_teacher_projection",cfg.vision_teacher_dim,d);

    if(c.video_object_learned_queries){VideoObjectTrackerWeights x;x.queries=r.f("video_object_tracker.queries",{cfg.object_slots,d});x.query_norm_weight=r.f("video_object_tracker.query_norm.weight",{d});x.query_norm_bias=r.f("video_object_tracker.query_norm.bias",{d});x.token_norm_weight=r.f("video_object_tracker.token_norm.weight",{d});x.token_norm_bias=r.f("video_object_tracker.token_norm.bias",{d});if(c.video_object_spatial_coordinates)x.position_projection_weight=r.f("video_object_tracker.position_projection.weight",{d,2});all.object_tracker=std::move(x);}
    const auto trajectory=[&](const std::string&p,bool pair){ObjectTrajectoryBindingWeights x;x.identity_norm_weight=r.f(p+".identity_norm.weight",{d});x.identity_norm_bias=r.f(p+".identity_norm.bias",{d});x.trajectory_norm_weight=r.f(p+".trajectory_norm.weight",{3*d+12});x.trajectory_norm_bias=r.f(p+".trajectory_norm.bias",{3*d+12});x.query_weight=r.f(p+".query.weight",{d,d});x.key_weight=r.f(p+".key.weight",{d,d});x.value_weight=r.f(p+".value.weight",{d,3*d+12});x.output_weight=r.f(p+".output.weight",{d,pair?4*d:d});return x;};
    if(cfg.object_trajectory_binding){all.trajectory=trajectory("video_object_trajectory_binding",false);all.trajectory->validate(d,false);}
    if(cfg.object_pair_trajectory_binding){all.pair_trajectory=trajectory("video_object_pair_trajectory_binding",true);all.pair_trajectory->validate(d,true);}
    if(cfg.descriptor_trajectory_binding){
        DescriptorConditionedDenseTrajectoryWeights x;x.feature_norm_weight=r.d("video_descriptor_trajectory_binding.feature_norm.weight",{d});x.feature_norm_bias=r.d("video_descriptor_trajectory_binding.feature_norm.bias",{d});x.query_weight=r.d("video_descriptor_trajectory_binding.query.weight",{d,d});x.key_weight=r.d("video_descriptor_trajectory_binding.key.weight",{d,d});x.trajectory_norm_weight=r.d("video_descriptor_trajectory_binding.trajectory_norm.weight",{3*d+12});x.trajectory_norm_bias=r.d("video_descriptor_trajectory_binding.trajectory_norm.bias",{3*d+12});x.value_weight=r.d("video_descriptor_trajectory_binding.value.weight",{d,3*d+12});x.output_weight=r.d("video_descriptor_trajectory_binding.output.weight",{d,4*d});
        if(c.video_descriptor_object_memory){const auto vw=c.video_descriptor_object_memory_contrast_visibility?4U:2U;x.memory_visibility_weight=r.d("video_descriptor_trajectory_binding.memory_visibility.weight",{1,vw});x.memory_visibility_bias=r.d("video_descriptor_trajectory_binding.memory_visibility.bias",{1});x.memory_output_weight=r.d("video_descriptor_trajectory_binding.memory_output.weight",{d,19});if(c.video_descriptor_object_memory_contrast_readout)x.memory_contrast_output_weight=r.d("video_descriptor_trajectory_binding.memory_contrast_output.weight",{d,8});if(c.video_descriptor_object_memory_temporal_relative_readout)x.memory_temporal_relative_output_weight=r.d("video_descriptor_trajectory_binding.memory_temporal_relative_output.weight",{d,19});if(c.video_descriptor_object_memory_query_gate){x.memory_gate_weight=r.d("video_descriptor_trajectory_binding.memory_gate.weight",{1,d});x.memory_gate_bias=r.d("video_descriptor_trajectory_binding.memory_gate.bias",{1});}if(c.video_descriptor_object_memory_reliability_gate){x.memory_reliability_gate_weight=r.d("video_descriptor_trajectory_binding.memory_reliability_gate.weight",{1,19});x.memory_reliability_gate_bias=r.d("video_descriptor_trajectory_binding.memory_reliability_gate.bias",{1});}}
        const DescriptorConditionedDenseTrajectoryConfig dc{d,c.video_descriptor_pair_centered_queries,c.video_descriptor_persistent_identity_state,c.video_descriptor_object_memory,c.video_descriptor_object_memory_scale,c.video_descriptor_object_memory_query_gate,c.video_descriptor_object_memory_reliability_gate,c.video_descriptor_object_memory_contrast_visibility,c.video_descriptor_object_memory_contrast_readout,c.video_descriptor_object_memory_temporal_relative_visibility,c.video_descriptor_object_memory_temporal_relative_readout};x.validate(dc);all.descriptor_trajectory=std::move(x);
    }
    if(cfg.camera_pose_dim&&cfg.spatial_relation_classes){const auto fd=cfg.camera_pose_dim+16,sd=3*cfg.camera_pose_dim+28;VideoSpatialGeometryWeights x;x.frame_norm_weight=r.f("video_spatial_geometry_reasoner.frame.0.weight",{fd});x.frame_norm_bias=r.f("video_spatial_geometry_reasoner.frame.0.bias",{fd});x.frame_linear1_weight=r.f("video_spatial_geometry_reasoner.frame.1.weight",{64,fd});x.frame_linear1_bias=r.f("video_spatial_geometry_reasoner.frame.1.bias",{64});x.frame_linear2_weight=r.f("video_spatial_geometry_reasoner.frame.3.weight",{64,64});x.frame_linear2_bias=r.f("video_spatial_geometry_reasoner.frame.3.bias",{64});x.stereo_norm_weight=r.f("video_spatial_geometry_reasoner.stereo_pair.0.weight",{sd});x.stereo_norm_bias=r.f("video_spatial_geometry_reasoner.stereo_pair.0.bias",{sd});x.stereo_linear1_weight=r.f("video_spatial_geometry_reasoner.stereo_pair.1.weight",{64,sd});x.stereo_linear1_bias=r.f("video_spatial_geometry_reasoner.stereo_pair.1.bias",{64});x.stereo_linear2_weight=r.f("video_spatial_geometry_reasoner.stereo_pair.3.weight",{32,64});x.stereo_linear2_bias=r.f("video_spatial_geometry_reasoner.stereo_pair.3.bias",{32});x.validate(cfg.camera_pose_dim);all.spatial_geometry=std::move(x);}
    if(cfg.egomotion_classes){VideoEgomotionWeights x;x.convolution1_weight=r.f("video_egomotion_reasoner.frontend.0.weight",{32,24,7,7});x.convolution1_bias=r.f("video_egomotion_reasoner.frontend.0.bias",{32});x.convolution2_weight=r.f("video_egomotion_reasoner.frontend.2.weight",{64,32,5,5});x.convolution2_bias=r.f("video_egomotion_reasoner.frontend.2.bias",{64});x.convolution3_weight=r.f("video_egomotion_reasoner.frontend.4.weight",{64,64,3,3});x.convolution3_bias=r.f("video_egomotion_reasoner.frontend.4.bias",{64});x.output_weight=r.f("video_egomotion_reasoner.output.weight",{d,64});x.output_bias=r.f("video_egomotion_reasoner.output.bias",{d});x.validate(d);all.egomotion=std::move(x);}
    r.finish({"video_","modality_embedding."});w.validate(cfg);return all;
}

MosaicUnifiedMaterializedWeights materialize_mosaic_unified(
    const MosaicStateDict&s,const MosaicUnifiedConfig&c){
    c.validate();const auto d=c.omni.world_dim;
    MosaicUnifiedMaterializedWeights out;
    {
        Reader r(s);out.runtime.world_norm_weight=r.d("world_norm.weight",{d});out.runtime.world_norm_bias=r.d("world_norm.bias",{d});
        if(c.visual_text_retrieval_head){const auto q=static_cast<std::size_t>(c.visual_text_retrieval_dim);out.runtime.visual_text_retrieval_projection=ml(r,"visual_text_retrieval_projection",q,d,false);out.runtime.text_visual_retrieval_projection=ml(r,"text_visual_retrieval_projection",q,d,false);}
        r.finish({"world_norm.","visual_text_retrieval_projection.","text_visual_retrieval_projection."});
    }
    const auto front_config=MosaicOmniModalFrontendConfig::from_unified(c);
    out.frontends=materialize_modal_frontends(s,front_config);
    const ModalToWorldConfig modal_config{d,WorldConfig{c.omni.world_slots,d,c.omni.object_slots},c.omni.attention_heads};
    out.to_world.emplace(materialize_modal_to_world(s,modal_config));
    if(c.visual_teacher_slot_bridge)out.visual_teacher=materialize_visual_teacher_slot_bridge(s,d,static_cast<std::size_t>(c.visual_teacher_slot_rank));
    else require_disabled_state_dict_prefix(s,"visual_teacher_slot_bridge.");
    if(c.explicit_object_relation_grounder){out.relation_grounder=materialize_explicit_grounder(s,d,static_cast<std::size_t>(c.explicit_object_relation_rank));out.relation_head=materialize_explicit_relation_head(s,d,static_cast<std::size_t>(c.explicit_relation_classes));}
    else{require_disabled_state_dict_prefix(s,"explicit_object_relation_grounder.");require_disabled_state_dict_prefix(s,"explicit_relation_head.");}
    const MosaicOmniWorldPipelineConfig world_config{WorldConfig{c.omni.world_slots,d,c.omni.object_slots},c.omni.attention_heads,static_cast<std::size_t>(c.world_ffn_dim),static_cast<std::size_t>(c.world_rounds),c.omni.object_slots,c.video_explicit_temporal_delta_scale,c.audio_temporal_binary_head,c.video_object_pair_trajectory_binding,c.video_descriptor_trajectory_binding};
    out.world_pipeline=materialize_world_pipeline(s,c,world_config);
    if(c.cross_modal_evidence_head)out.cross_modal=materialize_cross_modal_evidence(s,c);
    else require_disabled_state_dict_prefix(s,"cross_modal_");
    const MosaicOmniTextPipelineConfig text_config{d,static_cast<std::size_t>(c.text_answerability_classes),static_cast<std::size_t>(c.text_epistemic_memory_slots),static_cast<std::size_t>(c.text_epistemic_supported_class),c.text_epistemic_output_threshold,c.text_answerability_mode,c.text_answerability_head};
    out.text_pipeline=materialize_text_pipeline(s,c,text_config);
    if(c.text_only_cross_memory_adapter)out.text_cross_memory=materialize_text_cross_memory(s,c.text.model_dim);else require_disabled_state_dict_prefix(s,"text_only_cross_memory.");
    if(c.text_only_hidden_cross_memory_adapter)out.text_hidden_cross_memory=materialize_text_hidden_cross_memory(s,c.text.model_dim);else require_disabled_state_dict_prefix(s,"text_only_hidden_cross_memory.");
    if(c.text_epistemic_output_rank)out.text_epistemic_output=materialize_text_epistemic_output(s,c.text.model_dim,static_cast<std::size_t>(c.text_epistemic_output_rank));else require_disabled_state_dict_prefix(s,"text_epistemic_output.");
    if(c.text_answerability_head&&c.text_answerability_mode!="pooled"){
        const auto&mode=c.text_answerability_mode;
        const bool contextual=mode=="contextual-cross"||mode=="consistency-cross"||mode=="body-cross";
        const bool consistency=mode=="consistency-cross"||mode=="core-lexical-consistency-cross";
        const bool body=mode=="body-cross"||mode=="core-body-cross"||mode=="core-compact-body-cross"||mode=="core-projected-compact-body-cross"||mode=="core-lexical-compact-body-cross"||mode=="core-lexical-consistency-cross";
        const bool projected=mode=="core-projected-compact-body-cross"||mode=="core-lexical-compact-body-cross"||mode=="core-lexical-consistency-cross";
        const bool lexical=mode=="core-lexical-compact-body-cross"||mode=="core-lexical-consistency-cross";
        TextAnswerabilityConfig ac{d,c.omni.attention_heads,projected?c.text.model_dim:d,lexical?answerability_ngram_widths.size():0,static_cast<std::size_t>(c.text_answerability_classes),contextual,consistency,body};
        out.text_answerability=materialize_text_answerability(s,ac);
    }
    if(!c.text_answerability_head)require_disabled_state_dict_prefix(s,"text_answerability_head.");
    if(c.narrative_evidence_head)out.narrative=materialize_narrative_weights(s,c.text.model_dim,static_cast<std::size_t>(c.narrative_evidence_hidden_dim),d);
    else{require_disabled_state_dict_prefix(s,"narrative_continuity_head.");require_disabled_state_dict_prefix(s,"narrative_evidence_to_world.");}
    if(c.long_video_world_accumulator)out.long_video=materialize_long_video_weights(s,c.omni,c.long_video_transition_features);
    else require_disabled_state_dict_prefix(s,"long_video_accumulator.");
    out.video=materialize_video_pipeline(s,c);
    return out;
}

}  // namespace swegca::world
