#include "checkpoint/canonical_symbolic_json.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

using namespace swegca::checkpoint;

namespace {
Symbolic value(SymbolicKind kind) { auto out = std::make_shared<SymbolicValue>(); out->kind = kind; return out; }
Symbolic text(std::string v) { auto out = value(SymbolicKind::string); out->text = std::move(v); return out; }
Symbolic integer(std::int64_t v) { auto out = value(SymbolicKind::integer); out->integer = v; return out; }
Symbolic real(double v) { auto out = value(SymbolicKind::real); out->real = v; return out; }
Symbolic boolean(bool v) { auto out = value(SymbolicKind::boolean); out->boolean = v; return out; }
bool rejects(const Symbolic& input, const CanonicalSymbolicJsonLimits& limits = {}) {
    try { (void)canonical_symbolic_json(input, limits); }
    catch (const RestrictedPickleError&) { return true; }
    return false;
}
}

int main() {
    auto root = value(SymbolicKind::dictionary);
    auto list = value(SymbolicKind::list);
    list->sequence = {value(SymbolicKind::none), boolean(true), integer(-7), real(1.0),
                      text("줄\n\"인용\"\\끝\t\x01")};
    root->entries = {{text("한글"), list}, {text("a"), real(1e-7)}, {text("Z"), real(1e-4)}};
    assert(canonical_symbolic_json(root) ==
           "{\"Z\":0.0001,\"a\":1e-07,\"한글\":[null,true,-7,1.0,\"줄\\n\\\"인용\\\"\\\\끝\\t\\u0001\"]}");

    // These expected strings were produced by Python 3 json.dumps with
    // ensure_ascii=False, sort_keys=True and compact separators.
    struct FloatCase { double value; const char* expected; };
    for (const auto test : {
             FloatCase{0.0, "0.0"}, FloatCase{-0.0, "-0.0"},
             FloatCase{1.0, "1.0"}, FloatCase{1e15, "1000000000000000.0"},
             FloatCase{1e16, "1e+16"}, FloatCase{1e-4, "0.0001"},
             FloatCase{1e-5, "1e-05"}, FloatCase{1.2345678901234567, "1.2345678901234567"},
             FloatCase{1.2e100, "1.2e+100"}, FloatCase{1.2e-100, "1.2e-100"},
             FloatCase{9.999999999999999e-5, "9.999999999999999e-05"},
         }) assert(canonical_symbolic_json(real(test.value)) == test.expected);

    auto unicode_order = value(SymbolicKind::dictionary);
    unicode_order->entries = {{text("😀"), integer(3)}, {text("é"), integer(2)}, {text("a"), integer(1)}};
    assert(canonical_symbolic_json(unicode_order) == "{\"a\":1,\"é\":2,\"😀\":3}");

    auto duplicate = value(SymbolicKind::dictionary);
    duplicate->entries = {{text("x"), integer(1)}, {text("x"), integer(2)}};
    assert(rejects(duplicate));
    auto non_string = value(SymbolicKind::dictionary);
    non_string->entries = {{integer(1), integer(2)}};
    assert(rejects(non_string));

    assert(rejects(text(std::string("\xc0\x80", 2))));       // overlong NUL
    assert(rejects(text(std::string("\xed\xa0\x80", 3)))); // UTF-8 surrogate
    assert(rejects(real(std::numeric_limits<double>::infinity())));
    assert(rejects(real(std::numeric_limits<double>::quiet_NaN())));

    for (const auto kind : {SymbolicKind::tuple, SymbolicKind::ordered_dictionary,
                            SymbolicKind::global, SymbolicKind::storage, SymbolicKind::tensor})
        assert(rejects(value(kind)));

    auto cycle = value(SymbolicKind::list); cycle->sequence.push_back(cycle);
    assert(rejects(cycle));
    assert(rejects({}));

    CanonicalSymbolicJsonLimits small;
    small.maximum_output_bytes = 3;
    assert(rejects(text("abcd"), small));
    small = {};
    small.maximum_depth = 1;
    auto nested = value(SymbolicKind::list);
    auto nested_again = value(SymbolicKind::list);
    nested_again->sequence.push_back(integer(1)); nested->sequence.push_back(nested_again);
    assert(rejects(nested, small));

    std::cout << "canonical symbolic JSON tests passed\n";
}
