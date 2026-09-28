#pragma once

#include <array>
#include <cstdint>

namespace swegca::world::test_fixture {

// Generated from pinned mosaic_recurrent_cognition.py SHA-256
// 5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e
// with PyTorch 2.14.0+cu130 CPU, one thread, deterministic algorithms.
inline constexpr auto source_sha256 = "5ea32f8d6b97ae8db013e09b1cb4d42f49e9c1cd1794d8d8f5bad40ea519ef6e";

inline constexpr std::array<float, 12> weight_role_embeddings{{
    -0.391304344F, -0.347826093F, -0.304347813F, -0.260869563F, -0.217391297F, -0.173913047F, -0.130434781F, -0.0869565234F,
    -0.0434782617F, 0.0F, 0.0434782617F, 0.0869565234F
}};

inline constexpr std::array<float, 1> weight_halt_evidence_scale{{
    0.200000003F
}};

inline constexpr std::array<float, 4> weight_cell_attention_norm_weight{{
    -0.119999997F, -0.0799999982F, -0.0399999991F, 0.0F
}};

inline constexpr std::array<float, 4> weight_cell_attention_norm_bias{{
    0.0F, 0.0384615399F, 0.0769230798F, 0.115384616F
}};

inline constexpr std::array<float, 48> weight_cell_attention_in_proj_weight{{
    0.111111112F, 0.148148149F, 0.185185179F, 0.222222224F, 0.259259254F, 0.296296299F, 0.333333343F, -0.333333343F,
    -0.296296299F, -0.259259254F, -0.222222224F, -0.185185179F, -0.148148149F, -0.111111112F, -0.0740740746F, -0.0370370373F,
    0.0F, 0.0370370373F, 0.0740740746F, 0.111111112F, 0.148148149F, 0.185185179F, 0.222222224F, 0.259259254F,
    0.296296299F, 0.333333343F, -0.333333343F, -0.296296299F, -0.259259254F, -0.222222224F, -0.185185179F, -0.148148149F,
    -0.111111112F, -0.0740740746F, -0.0370370373F, 0.0F, 0.0370370373F, 0.0740740746F, 0.111111112F, 0.148148149F,
    0.185185179F, 0.222222224F, 0.259259254F, 0.296296299F, 0.333333343F, -0.333333343F, -0.296296299F, -0.259259254F
}};

inline constexpr std::array<float, 12> weight_cell_attention_in_proj_bias{{
    0.214285716F, 0.25F, 0.285714298F, 0.321428567F, -0.321428567F, -0.285714298F, -0.25F, -0.214285716F,
    -0.178571433F, -0.142857149F, -0.107142858F, -0.0714285746F
}};

inline constexpr std::array<float, 16> weight_cell_attention_out_proj_weight{{
    0.310344815F, -0.310344815F, -0.275862068F, -0.241379306F, -0.206896558F, -0.172413796F, -0.137931034F, -0.103448279F,
    -0.068965517F, -0.0344827585F, 0.0F, 0.0344827585F, 0.068965517F, 0.103448279F, 0.137931034F, 0.172413796F
}};

inline constexpr std::array<float, 4> weight_cell_attention_out_proj_bias{{
    -0.233333334F, -0.200000003F, -0.166666672F, -0.13333334F
}};

inline constexpr std::array<float, 4> weight_cell_mlp_norm_weight{{
    -0.129032254F, -0.0967741907F, -0.0645161271F, -0.0322580636F
}};

inline constexpr std::array<float, 4> weight_cell_mlp_norm_bias{{
    -0.03125F, 0.0F, 0.03125F, 0.0625F
}};

inline constexpr std::array<float, 24> weight_cell_mlp_in_weight{{
    0.0606060624F, 0.0909090936F, 0.121212125F, 0.151515156F, 0.181818187F, 0.212121218F, 0.24242425F, 0.272727281F,
    -0.272727281F, -0.24242425F, -0.212121218F, -0.181818187F, -0.151515156F, -0.121212125F, -0.0909090936F, -0.0606060624F,
    -0.0303030312F, 0.0F, 0.0303030312F, 0.0606060624F, 0.0909090936F, 0.121212125F, 0.151515156F, 0.181818187F
}};

inline constexpr std::array<float, 12> weight_cell_mlp_out_weight{{
    0.14705883F, 0.176470593F, 0.205882356F, 0.235294119F, 0.264705896F, -0.264705896F, -0.235294119F, -0.205882356F,
    -0.176470593F, -0.14705883F, -0.117647059F, -0.0882352963F
}};

inline constexpr std::array<float, 4> weight_cell_update_gate_weight{{
    0.22857143F, 0.257142872F, -0.257142872F, -0.22857143F
}};

inline constexpr std::array<float, 1> weight_cell_update_gate_bias{{
    -0.222222224F
}};

inline constexpr std::array<float, 4> weight_final_norm_weight{{
    -0.135135129F, -0.108108111F, -0.0810810775F, -0.0540540554F
}};

inline constexpr std::array<float, 4> weight_final_norm_bias{{
    -0.0526315793F, -0.0263157897F, 0.0F, 0.0263157897F
}};

inline constexpr std::array<float, 4> weight_halt_head_weight{{
    0.150000006F, -0.119999997F, 0.0799999982F, 0.0500000007F
}};

inline constexpr std::array<float, 1> weight_halt_head_bias{{
    0.0F
}};

inline constexpr std::array<float, 16> input_semantic{{
    -0.727272749F, -0.636363626F, -0.545454562F, -0.454545468F, -0.363636374F, -0.272727281F, -0.181818187F, -0.0909090936F,
    0.0F, 0.0909090936F, 0.181818187F, 0.272727281F, 0.363636374F, 0.454545468F, 0.545454562F, 0.636363626F
}};

inline constexpr std::array<float, 8> input_executive{{
    -0.230769232F, -0.15384616F, -0.0769230798F, 0.0F, 0.0769230798F, 0.15384616F, 0.230769232F, 0.307692319F
}};

inline constexpr std::array<float, 8> input_scratch{{
    0.142857149F, 0.285714298F, 0.428571433F, 0.571428597F, 0.714285731F, 0.857142866F, 1.0F, 1.14285719F
}};

inline constexpr std::array<float, 16> input_evidence{{
    -0.444444448F, -0.333333343F, -0.222222224F, -0.111111112F, 0.0F, 0.111111112F, 0.222222224F, 0.333333343F,
    0.444444448F, 0.555555582F, 0.666666687F, 0.777777791F, 0.888888896F, -0.888888896F, -0.777777791F, -0.666666687F
}};

inline constexpr std::array<std::uint8_t, 4> input_mask{{
    1, 0, 1, 1
}};

inline constexpr std::array<float, 2> input_coverage{{
    0.899999976F, 0.200000003F
}};

inline constexpr std::array<float, 2> input_confidence{{
    1.0F, 0.5F
}};

inline constexpr std::array<float, 4> input_keyweights{{
    2.0F, 3.0F, 4.0F, 5.0F
}};

inline constexpr auto expected_eval_state_sha256 = "9c3d1595d90b97308a50963aec83286ef5e29fe4a6d1af19e90969e80f964964";
inline constexpr std::array<float, 32> expected_eval_state{{
    0.154038221F, -0.00969990529F, -0.0413565822F, -0.0370890759F, 0.15403828F, -0.00969986245F, -0.0413565971F, -0.0370890759F,
    0.162955537F, -0.0243217852F, -0.0431518704F, -0.032148134F, 0.162955508F, -0.0243217945F, -0.0431518853F, -0.032148134F,
    0.156141788F, -0.0129199196F, -0.041779317F, -0.036038585F, 0.16551441F, -0.029143529F, -0.0436666086F, -0.0304176714F,
    0.148348331F, -0.00156697235F, -0.0402131788F, -0.0396418162F, 0.155681238F, -0.0121699926F, -0.0416889004F, -0.0362896621F
}};

inline constexpr std::array<std::uint64_t, 2> expected_eval_cycles{{
    2, 3
}};

inline constexpr std::array<float, 2> expected_eval_logits_0{{
    1.68612111F, -1.77107215F
}};

inline constexpr std::array<float, 2> expected_eval_logits_1{{
    1.66736698F, -1.78884912F
}};

inline constexpr std::array<float, 2> expected_eval_logits_2{{
    1.66736698F, -1.80661285F
}};

inline constexpr std::array<float, 2> expected_eval_probabilities_0{{
    0.843713343F, 0.145409048F
}};

inline constexpr std::array<float, 2> expected_eval_probabilities_1{{
    0.841224492F, 0.143213883F
}};

inline constexpr std::array<float, 2> expected_eval_probabilities_2{{
    0.841224492F, 0.141047984F
}};

inline constexpr auto expected_training_state_sha256 = "9c3d1595d90b97308a50963aec83286ef5e29fe4a6d1af19e90969e80f964964";
inline constexpr std::array<float, 32> expected_training_state{{
    0.154038221F, -0.00969990529F, -0.0413565822F, -0.0370890759F, 0.15403828F, -0.00969986245F, -0.0413565971F, -0.0370890759F,
    0.162955537F, -0.0243217852F, -0.0431518704F, -0.032148134F, 0.162955508F, -0.0243217945F, -0.0431518853F, -0.032148134F,
    0.156141788F, -0.0129199196F, -0.041779317F, -0.036038585F, 0.16551441F, -0.029143529F, -0.0436666086F, -0.0304176714F,
    0.148348331F, -0.00156697235F, -0.0402131788F, -0.0396418162F, 0.155681238F, -0.0121699926F, -0.0416889004F, -0.0362896621F
}};

inline constexpr std::array<std::uint64_t, 2> expected_training_cycles{{
    2, 3
}};

inline constexpr std::array<float, 2> expected_training_logits_0{{
    1.68612111F, -1.77107215F
}};

inline constexpr std::array<float, 2> expected_training_logits_1{{
    1.66736698F, -1.78884912F
}};

inline constexpr std::array<float, 2> expected_training_logits_2{{
    1.64861023F, -1.80661285F
}};

inline constexpr std::array<float, 2> expected_training_probabilities_0{{
    0.843713343F, 0.145409048F
}};

inline constexpr std::array<float, 2> expected_training_probabilities_1{{
    0.841224492F, 0.143213883F
}};

inline constexpr std::array<float, 2> expected_training_probabilities_2{{
    0.838703096F, 0.141047984F
}};

inline constexpr auto expected_zero_state_sha256 = "d2529e9cfe2e65df9a066bd5d9dc2d0aada647ddb23aced84531d333f8a903b1";
inline constexpr std::array<float, 32> expected_zero_state{{
    0.162069455F, -0.0227213651F, -0.0429712683F, -0.0327142738F, 0.162069425F, -0.0227213632F, -0.0429713093F, -0.0327142738F,
    0.162069395F, -0.0227213819F, -0.0429713167F, -0.0327142812F, 0.16206941F, -0.0227213912F, -0.0429713093F, -0.0327142738F,
    0.164334834F, -0.026867412F, -0.0434268191F, -0.0312437732F, 0.164334863F, -0.026867412F, -0.0434268191F, -0.0312437601F,
    0.155507609F, -0.0118987728F, -0.0416521281F, -0.0363803022F, 0.155507609F, -0.0118987644F, -0.0416521244F, -0.0363803133F
}};

inline constexpr std::array<std::uint64_t, 2> expected_zero_cycles{{
    3, 3
}};

inline constexpr std::array<float, 2> expected_zero_logits_0{{
    -7.41862011F, -7.36938906F
}};

inline constexpr std::array<float, 2> expected_zero_logits_1{{
    -7.4373908F, -7.38816023F
}};

inline constexpr std::array<float, 2> expected_zero_logits_2{{
    -7.45617533F, -7.40694475F
}};

inline constexpr std::array<float, 2> expected_zero_probabilities_0{{
    0.000599616731F, 0.000629856135F
}};

inline constexpr std::array<float, 2> expected_zero_probabilities_1{{
    0.000588473049F, 0.000618150516F
}};

inline constexpr std::array<float, 2> expected_zero_probabilities_2{{
    0.000577528379F, 0.000606654212F
}};

}  // namespace swegca::world::test_fixture
