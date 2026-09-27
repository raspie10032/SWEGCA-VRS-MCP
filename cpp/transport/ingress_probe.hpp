#pragma once
// Diagnostic builds only. No content, identifiers or paths are emitted.
#ifdef SWEGCA_INGRESS_STAGE_PROBE
extern "C" void swegca_ingress_stage_probe(const char*) noexcept;
#define SWEGCA_INGRESS_STAGE(name) swegca_ingress_stage_probe(name)
#else
#define SWEGCA_INGRESS_STAGE(name) do {} while(false)
#endif
