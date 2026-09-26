#include "swegca_architecture/evidence_rules.hpp"
#include "vrs/connection.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

using namespace swegca::architecture;
using namespace swegca::architecture::kernel;
using namespace swegca::vrs;
static unsigned checks = 0;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
        std::abort(); \
    } \
} while (false)

template<class Exception, class Function> void expect_throw(Function operation) {
    bool caught = false;
    try { operation(); } catch (const Exception&) { caught = true; }
    CHECK(caught);
}
Digest id(std::uint64_t value) {
    Digest digest{};
    for (unsigned i = 0; i < 8; ++i) digest[i] = std::byte((value >> (8 * i)) & 255);
    return digest;
}
std::span<const std::byte> bytes(const std::string& value) {
    return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

struct Originals {
    std::filesystem::path directory;
    ExperienceBlock left, right;
    std::uint64_t sequence = 0;
    MemoryBudget memory{1 << 20};
    EvidenceRules rules = make_evidence_rules(EvidencePolicy{});
    static std::filesystem::path make_directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "swegca-connection-XXXXXX").string();
        const auto made = ::mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("test directory creation failed");
        return made;
    }
    Originals() : directory(make_directory()),
        left(ExperienceBlock::create(directory / "left.block", id(10001), 1 << 20)),
        right(ExperienceBlock::create(directory / "right.block", id(10002), 1 << 20)) {}
    ~Originals() { std::filesystem::remove_all(directory); }

    ExperienceEvidence sample(Digest hypothesis, unsigned axis, unsigned source,
        EvidenceOutcome outcome, bool expires = false, bool one_producer = false, bool one_context = false) {
        const std::string raw = "experiment=" + std::to_string(sequence) + ";axis=" +
            std::to_string(axis) + ";outcome=" + std::to_string(unsigned(outcome));
        auto& block = sequence % 2 ? left : right;
        EvidenceObservation value;
        value.hypothesis = hypothesis;
        value.source = id(source + 1);
        value.context = id(one_context ? 1 : source + 1001);
        value.producer = id(one_producer ? 1 : source + 2001);
        value.axis = axis;
        value.outcome = outcome;
        value.has_expiry = expires;
        value.expires_at = 10;
        return record_evidence(block, rules,
            {sequence++, 0, "session", "experiment", "text/plain", bytes(raw)}, value, memory);
    }
    void fill(Connection& connection, EvidenceOutcome outcome, bool expires = false) {
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned group = 0; group < 12; ++group)
                connection.append(sample(connection.identity(), axis, group, outcome, expires));
    }
};

unsigned used(const ConnectionRefinement& report, ObservationUse outcome) {
    return static_cast<unsigned>(std::count_if(report.samples().begin(), report.samples().end(),
        [=](const auto& value) { return value.use == outcome; }));
}

static bool emit_oracle = false;
void print_digest(const Digest& digest) {
    for (auto byte : digest) std::printf("%02x", std::to_integer<unsigned>(byte));
}
void oracle(const Connection& connection, const ConnectionRefinement& report) {
    if (!emit_oracle) return;
    std::printf("ORACLE {\"hypothesis\":\""); print_digest(connection.identity());
    std::printf("\",\"current_step\":%llu,\"observations\":[", (unsigned long long)report.current_step());
    bool first = true;
    for (const auto& sample : report.samples()) {
        const auto& value = connection.experiences()[sample.experience_index].value();
        if (!first) std::printf(",");
        first = false;
        std::printf("{\"address\":\""); print_digest(value.address);
        std::printf("\",\"source\":\""); print_digest(value.source);
        std::printf("\",\"context\":\""); print_digest(value.context);
        std::printf("\",\"producer\":\""); print_digest(value.producer);
        std::printf("\",\"axis\":%u,\"outcome\":%u,\"observed_at\":%llu,\"expires_at\":",
            value.axis, unsigned(value.outcome), (unsigned long long)value.observed_at);
        if (value.has_expiry) std::printf("%llu", (unsigned long long)value.expires_at);
        else std::printf("null");
        std::printf(",\"use\":%u}", unsigned(sample.use));
    }
    const auto& t = report.evidence();
    std::printf("],\"axis_support\":[");
    for (unsigned i = 0; i < 4; ++i) std::printf("%s%.17g", i ? "," : "", t.axis_support[i]);
    std::printf("],\"axis_refute\":[");
    for (unsigned i = 0; i < 4; ++i) std::printf("%s%.17g", i ? "," : "", t.axis_refute[i]);
    std::printf("],\"axis_sources\":[");
    for (unsigned i = 0; i < 4; ++i) std::printf("%s%u", i ? "," : "", t.axis_source_diversity[i]);
    std::printf("],\"source_diversity\":%u,\"context_diversity\":%u,\"recent_count\":%u,"
        "\"recent_sum\":%.17g,\"revision\":%llu,\"status\":%u,\"reason\":%u}\n",
        t.source_diversity, t.context_diversity, t.recent_count, t.recent_sum,
        (unsigned long long)t.revision, unsigned(report.result().verification().judgment().status()),
        unsigned(report.result().verification().judgment().reason()));
}

// Fail any allocation number in a refinement. The same owner is reused after
// every failure, so a partially changed strength/revision is observable.
class FailingMemory final : public std::pmr::memory_resource {
public:
    std::size_t until_failure = std::numeric_limits<std::size_t>::max();
    std::size_t maximum_request = 0;
    std::size_t request_limit = std::numeric_limits<std::size_t>::max();
private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        maximum_request = std::max(maximum_request, bytes);
        if (until_failure == 0 || bytes > request_limit) throw std::bad_alloc();
        --until_failure;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
};

int main(int argc, char** argv) {
    emit_oracle = argc == 2 && std::string_view(argv[1]) == "--oracle-jsonl";
    static_assert(!std::is_copy_constructible_v<Connection>);
    static_assert(!std::is_aggregate_v<ExperienceEvidence>);
    static_assert(!std::is_default_constructible_v<ExperienceEvidence>);
    static_assert(!std::is_move_constructible_v<Connection>);
    static_assert(!std::is_copy_constructible_v<ConnectionRefinement>);
    static_assert(std::is_nothrow_move_constructible_v<ConnectionRefinement>);
    const auto rules = make_evidence_rules(EvidencePolicy{});
    MemoryBudget memory(16 << 20);
    Originals originals;
    {
        FailingMemory upstream;
        MemoryBudget segmented_memory(16 << 20, &upstream);
        upstream.request_limit = 256 * sizeof(ExperienceEvidence);
        {
            Connection segmented(id(90), 1, rules, segmented_memory);
            std::vector<ExperienceEvidence> expected;
            std::vector<const ExperienceEvidence*> addresses;
            for (unsigned i = 0; i < 1030; ++i) {
                auto value = originals.sample(id(90), i % 4, i / 4, EvidenceOutcome::support);
                const auto snapshot = segmented.experiences();
                const auto before = segmented_memory.used();
                const auto revision = segmented.revision();
                const bool boundary = i == 0 || i == 8 || i == 24 || i == 56 ||
                    i == 120 || i == 248 || i == 504 || i == 760 || i == 1016;
                if (boundary) {
                    // Segment allocation, followed by directory growth when
                    // needed. A failed append must release both reservations.
                    for (std::size_t allowed = 0;; ++allowed) {
                        upstream.until_failure = allowed;
                        try { segmented.append(value); break; }
                        catch (const std::bad_alloc&) {
                            CHECK(allowed < 3);
                            CHECK(segmented_memory.used() == before);
                            CHECK(segmented.revision() == revision);
                            CHECK(segmented.strength() == 1);
                            CHECK(segmented.experiences().size() == i);
                        }
                    }
                    upstream.until_failure = std::numeric_limits<std::size_t>::max();
                } else segmented.append(value);
                expected.push_back(value);
                addresses.push_back(&segmented.experiences()[i]);
                CHECK(snapshot.size() == i);
                if (i) CHECK(&snapshot[i-1] == addresses[i-1]);
            }
            CHECK(upstream.maximum_request <= 256 * sizeof(ExperienceEvidence));
            const auto view = segmented.experiences();
            std::size_t i = 0;
            for (const auto& value : view) {
                CHECK(&value == addresses[i]);
                CHECK(value.original() == expected[i].original());
                CHECK(value.value().address == expected[i].value().address);
                CHECK(value.value().axis == expected[i].value().axis);
                ++i;
            }
            CHECK(i == 1030);
            const auto suffix = view.subspan(247);
            CHECK(suffix.size() == 783);
            CHECK(&suffix[1] == addresses[248]);
            CHECK(view.subspan(1030).empty());
            expect_throw<std::out_of_range>([&] { (void)view.subspan(1031); });
            upstream.request_limit = std::numeric_limits<std::size_t>::max();
            {
                std::optional<ExperienceSequence> parent;
                parent.emplace(segmented_memory);
                for(const auto& value:expected){parent->prepare_append();parent->commit_append(value);}
                {
                    const auto used=segmented_memory.used();
                    upstream.until_failure=0;
                    expect_throw<std::bad_alloc>([&]{(void)parent->snapshot(segmented_memory);});
                    upstream.until_failure=std::numeric_limits<std::size_t>::max();
                    CHECK(segmented_memory.used()==used);
                }
                ExperienceSequence child(segmented_memory);
                const auto baseline=segmented_memory.used();
                unsigned failed=0;
                for(std::size_t allowed=0;;++allowed){
                    upstream.until_failure=allowed;
                    try{child.share_prefix(*parent);break;}
                    catch(const std::bad_alloc&){
                        ++failed;CHECK(child.size()==0);CHECK(parent->size()==1030);
                        CHECK(segmented_memory.used()==baseline);CHECK(allowed<3);
                    }
                }
                upstream.until_failure=std::numeric_limits<std::size_t>::max();
                CHECK(failed==3);
                CHECK(segmented_memory.used()-baseline < 256*sizeof(ExperienceEvidence)+4096);
                std::printf("Shared prefix: %zu new bytes for 1030 experiences (%zu bytes of sealed values)\n",
                    segmented_memory.used()-baseline,expected.size()*sizeof(ExperienceEvidence));
                for(std::size_t n=0;n<expected.size();++n){
                    CHECK(child[n].original()==expected[n].original());
                    CHECK((&child[n]==&(*parent)[n])==(n<1016));
                }
                auto pinned=parent->snapshot(segmented_memory);
                CHECK(pinned.size()==1030);
                for(std::size_t n=0;n<expected.size();++n)CHECK(&pinned[n]==&(*parent)[n]);
                parent->prepare_append();parent->commit_append(expected[0]);
                CHECK(child.size()==1030);
                child.prepare_append();child.commit_append(expected[1]);
                CHECK((*parent)[1030].original()!=child[1030].original());
                parent.reset();
                auto moved=std::move(pinned);CHECK(pinned.size()==0&&moved.size()==1030);
                expect_throw<std::out_of_range>([&]{(void)moved[1030];});
                for(std::size_t n=0;n<expected.size();++n)CHECK(moved[n].original()==expected[n].original());
                for(std::size_t n=0;n<expected.size();++n)CHECK(child[n].original()==expected[n].original());
            }
            const auto report = segmented.refine(12345, 5);
            oracle(segmented, report);
            CHECK(report.samples().size() == 1030);
            CHECK(report.result().verification().judgment().status() == EvidenceStatus::accept);
            CHECK(segmented.strength() == 1.01);
            std::array<bool, 1030> seen{};
            for (const auto& sample : report.samples()) {
                CHECK(sample.experience_index < seen.size());
                CHECK(!seen[sample.experience_index]);
                seen[sample.experience_index] = true;
            }
        }
        CHECK(segmented_memory.used() == 0);
    }
    {
        Connection positive(id(1), 0.999, rules, memory);
        originals.fill(positive, EvidenceOutcome::support);
        const auto before = positive.revision();
        const auto report = positive.refine(12345, 5);
        oracle(positive, report);
        CHECK(report.connection() == positive.identity());
        CHECK(report.before_revision() == before);
        CHECK(report.after_revision() == positive.revision());
        CHECK(positive.revision() == before + 1);
        CHECK(report.seed() == 12345 && report.current_step() == 5);
        CHECK(report.result().verification().judgment().status() == EvidenceStatus::accept);
        CHECK(positive.strength() == 0.999 * 1.01);
        CHECK(report.result().strength().evidence_eligible());
        CHECK(report.evidence().revision == 48);
        CHECK(report.evidence().source_diversity == 12);
        CHECK(report.evidence().context_diversity == 12);
        CHECK(report.evidence().recent_count == 6 && report.evidence().recent_sum == 6);
        for (unsigned axis = 0; axis < 4; ++axis) {
            CHECK(report.evidence().axis_support[axis] == 12);
            CHECK(report.evidence().axis_refute[axis] == 0);
            CHECK(report.evidence().axis_source_diversity[axis] == 12);
        }
        bool reordered = false;
        std::array<bool, 48> seen{};
        Connection reread(positive.identity(), 0.999, rules, memory);
        for (std::size_t i = 0; i < report.samples().size(); ++i) {
            const auto index = report.samples()[i].experience_index;
            CHECK(index < seen.size());
            CHECK(!seen[index]);
            seen[index] = true;
            reordered |= index != i;
            const auto& experience = positive.experiences()[index];
            auto& block = experience.original().block == originals.left.identity() ? originals.left : originals.right;
            const auto stored = block.read(experience.original(), 4096, memory);
            const auto decoded = decode_evidence(rules, stored);
            CHECK(stored.location().digest == decoded.value().address);
            CHECK(decoded.value().outcome == experience.value().outcome);
            CHECK(decoded.value().hypothesis == positive.identity());
            reread.append(decoded);
            const auto payload = evidence_payload(stored);
            CHECK(payload.media_type == "text/plain");
            CHECK(std::string_view(reinterpret_cast<const char*>(payload.content.data()), payload.content.size()).starts_with("experiment="));
        }
        CHECK(reordered);
        const auto from_disk = reread.refine(12345, 5);
        CHECK(from_disk.result().verification().judgment().status() == EvidenceStatus::accept);
        CHECK(reread.strength() == positive.strength());
        const auto repeat = positive.refine(12345, 5);
        CHECK(repeat.evidence().revision == 48); // no prior tally was added
        CHECK(repeat.result().strength().previous() == report.result().strength().current());
        CHECK(std::equal(report.samples().begin(), report.samples().end(), repeat.samples().begin(),
            [](const auto& a, const auto& b) { return a.experience_index == b.experience_index && a.use == b.use; }));
        const auto other_seed = positive.refine(54321, 5);
        CHECK(!std::equal(report.samples().begin(), report.samples().end(), other_seed.samples().begin(),
            [](const auto& a, const auto& b) { return a.experience_index == b.experience_index; }));
        CHECK(used(other_seed, ObservationUse::applied) == 48);
    }
    CHECK(memory.used() == 0);
    {
        // Payload and numeric observation values are in the same checksummed
        // record. Preserve binary/NUL content and reject malformed envelopes.
        EvidenceObservation value;
        value.hypothesis = id(20); value.source = id(1); value.context = id(2); value.producer = id(3);
        value.axis = 3; value.outcome = EvidenceOutcome::insufficient;
        value.producer_confidence = 0.625; value.observed_at = 42;
        const std::string binary("first\0원문\0last", sizeof("first\0원문\0last") - 1);
        const OriginalExperienceView raw{987, 42, "session-z", "source-z", "application/octet-stream", bytes(binary)};
        const auto evidence = record_evidence(originals.left, rules, raw, value, memory);
        const auto stored = originals.left.read(evidence.original(), 4096, memory);
        const auto decoded = decode_evidence(rules, stored);
        CHECK(decoded.value().producer_confidence == 0.625);
        CHECK(decoded.value().observed_at == 42);
        CHECK(decoded.value().axis == 3);
        CHECK(decoded.value().outcome == EvidenceOutcome::insufficient);
        const auto replay = evidence_payload(stored);
        CHECK(replay.sequence == 987 && replay.observed_at_ns == 42);
        CHECK(replay.session == "session-z" && replay.source == "source-z");
        CHECK(replay.media_type == "application/octet-stream");
        CHECK(std::ranges::equal(replay.content, bytes(binary)));
        const auto raw_record = originals.left.append(raw);
        const auto raw_stored = originals.left.read(raw_record, 4096, memory);
        expect_throw<std::invalid_argument>([&] { (void)decode_evidence(rules, raw_stored); });
        std::vector<std::byte> corrupt(stored.view().content.begin(), stored.view().content.end());
        auto envelope = stored.view();
        envelope.content = corrupt;
        for (const auto offset : {0U, 156U, 157U, 158U, 160U, 168U}) {
            const auto previous = corrupt[offset];
            corrupt[offset] = std::byte{255};
            const auto malformed = originals.left.append(envelope);
            const auto read = originals.left.read(malformed, 4096, memory);
            expect_throw<std::invalid_argument>([&] { (void)decode_evidence(rules, read); });
            corrupt[offset] = previous;
        }
        const auto old_size = originals.left.inspect().complete_records;
        value.observed_at = 43;
        expect_throw<std::invalid_argument>([&] { (void)record_evidence(originals.left, rules, raw, value, memory); });
        CHECK(originals.left.inspect().complete_records == old_size);
    }
    CHECK(memory.used() == 0);
    {
        Connection negative(id(2), 1.0, rules, memory);
        originals.fill(negative, EvidenceOutcome::refute);
        const auto report = negative.refine(7, 5);
        oracle(negative, report);
        CHECK(report.result().verification().judgment().status() == EvidenceStatus::reject);
        CHECK(negative.strength() == 0.995);
        CHECK(report.evidence().recent_sum == 0);
        CHECK(!report.result().strength().evidence_eligible());
        Connection uncertain(id(3), 1.25, rules, memory);
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned group = 0; group < 12; ++group)
                uncertain.append(originals.sample(id(3), axis, group,
                    group % 2 ? EvidenceOutcome::support : EvidenceOutcome::refute));
        const auto abstain = uncertain.refine(17, 5);
        oracle(uncertain, abstain);
        CHECK(abstain.result().verification().judgment().status() == EvidenceStatus::abstain);
        CHECK(abstain.result().strength().valid());
        CHECK(uncertain.strength() == 1.25);
    }
    {
        Connection repeated(id(4), 1, rules, memory);
        originals.fill(repeated, EvidenceOutcome::support);
        for (unsigned i = 0; i < 48; ++i) {
            const auto copy = repeated.experiences()[i];
            repeated.append(copy);
        }
        const auto report = repeated.refine(50, 5);
        oracle(repeated, report);
        CHECK(used(report, ObservationUse::duplicate) == 48);
        CHECK(used(report, ObservationUse::applied) == 48);
        CHECK(report.evidence().revision == 48);
        CHECK(report.evidence().axis_support[0] == 12);

        Connection correlated(id(5), 1, rules, memory);
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned i = 0; i < 12; ++i)
                correlated.append(originals.sample(id(5), axis, 0, EvidenceOutcome::support));
        const auto discounted = correlated.refine(51, 5);
        oracle(correlated, discounted);
        CHECK(discounted.evidence().revision == 48);
        CHECK(discounted.result().verification().judgment().effective_sample_size() == 4);
        CHECK(discounted.evidence().source_diversity == 1);
        CHECK(discounted.result().verification().judgment().reason() == EvidenceReason::minimum_effective_samples);
        CHECK(correlated.strength() == 1);
    }
    {
        Connection changing(id(6), 1, rules, memory);
        originals.fill(changing, EvidenceOutcome::support, true);
        const auto at_expiry = changing.refine(2, 10);
        oracle(changing, at_expiry);
        CHECK(used(at_expiry, ObservationUse::applied) == 48);
        CHECK(changing.strength() == 1.01);
        originals.fill(changing, EvidenceOutcome::refute);
        const auto after_expiry = changing.refine(2, 11);
        oracle(changing, after_expiry);
        CHECK(used(after_expiry, ObservationUse::expired) == 48);
        CHECK(after_expiry.evidence().axis_support[0] == 0);
        CHECK(after_expiry.evidence().axis_refute[0] == 12);
        CHECK(after_expiry.evidence().revision == 48);
        CHECK(after_expiry.result().verification().judgment().status() == EvidenceStatus::reject);
        CHECK(changing.strength() == 1.01 * 0.995);
        CHECK(changing.experiences().size() == 96); // expired originals retained
    }
    {
        Connection unknown(id(7), 0.5, rules, memory);
        const auto empty = unknown.refine(1, 0);
        oracle(unknown, empty);
        CHECK(empty.result().strength().valid());
        CHECK(empty.result().verification().judgment().reason() == EvidenceReason::minimum_effective_samples);
        CHECK(unknown.revision() == 1 && unknown.strength() == 0.5);
        originals.fill(unknown, EvidenceOutcome::insufficient);
        const auto report = unknown.refine(3, 5);
        oracle(unknown, report);
        CHECK(used(report, ObservationUse::insufficient) == 48);
        CHECK(report.evidence().revision == 0);
        CHECK(unknown.strength() == 0.5);
        CHECK(unknown.experiences().size() == 48);
        const auto before = unknown.revision();
        const auto bad = originals.sample(id(1000), 0, 0, EvidenceOutcome::support);
        expect_throw<std::invalid_argument>([&] { unknown.append(bad); });
        auto bad_value = unknown.experiences()[0].value();
        const OriginalExperienceView raw{0, 0, "session", "experiment", "text/plain", {}};
        expect_throw<std::invalid_argument>([&] { (void)record_evidence(originals.left, rules, raw, bad_value, memory); });
        bad_value.address = {};
        bad_value.producer_confidence = std::numeric_limits<double>::quiet_NaN();
        expect_throw<std::invalid_argument>([&] { (void)record_evidence(originals.left, rules, raw, bad_value, memory); });
        CHECK(unknown.revision() == before && unknown.experiences().size() == 48);
    }
    {
        Connection mixed(id(12), 1, rules, memory);
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned group = 0; group < 12; ++group)
                for (unsigned item = 0; item < 3; ++item)
                    mixed.append(originals.sample(id(12), axis, group,
                        item < 2 ? EvidenceOutcome::support : EvidenceOutcome::refute));
        for (auto seed : {13U, 255U, 1024U}) {
            const auto report = mixed.refine(seed, 5);
            oracle(mixed, report);
            CHECK(report.evidence().revision == 144);
            CHECK(report.result().verification().judgment().effective_sample_size() > 47.999999999);
            CHECK(report.result().verification().judgment().effective_sample_size() < 48.000000001);
            CHECK(report.result().verification().judgment().status() == EvidenceStatus::abstain);
            CHECK(mixed.strength() == 1);
        }
        Connection one_producer(id(13), 1, rules, memory);
        Connection one_context(id(14), 1, rules, memory);
        for (unsigned axis = 0; axis < 4; ++axis)
            for (unsigned group = 0; group < 12; ++group) {
                one_producer.append(originals.sample(id(13), axis, group, EvidenceOutcome::support, false, true));
                one_context.append(originals.sample(id(14), axis, group, EvidenceOutcome::support, false, false, true));
            }
        const auto producer_report = one_producer.refine(1, 5);
        oracle(one_producer, producer_report);
        CHECK(producer_report.result().verification().judgment().reason() == EvidenceReason::source_diversity);
        CHECK(one_producer.strength() == 1);
        const auto context_report = one_context.refine(1, 5);
        oracle(one_context, context_report);
        CHECK(context_report.result().verification().judgment().reason() == EvidenceReason::context_diversity);
        CHECK(one_context.strength() == 1);
    }
    {
        // Two Main-owned connections can be refined on separate workers,
        // sharing one memory budget. No verdict for A is applied to B.
        Connection a(id(8), 1, rules, memory), b(id(9), 2, rules, memory);
        originals.fill(a, EvidenceOutcome::support);
        originals.fill(b, EvidenceOutcome::refute);
        std::thread one([&] { (void)a.refine(1, 5); });
        std::thread two([&] { (void)b.refine(1, 5); });
        one.join(); two.join();
        CHECK(a.strength() == 1.01 && b.strength() == 2 * 0.995);
    }
    CHECK(memory.used() == 0);
    {
        FailingMemory upstream;
        MemoryBudget limited(16 << 20, &upstream);
        Connection connection(id(10), 0.75, rules, limited);
        originals.fill(connection, EvidenceOutcome::support);
        const auto baseline = limited.used();
        const auto revision = connection.revision();
        bool completed = false;
        unsigned failed_allocations = 0;
        for (std::size_t allowed = 0; allowed < 1000; ++allowed) {
            upstream.until_failure = allowed;
            try {
                const auto report = connection.refine(11, 5);
                CHECK(report.result().strength().valid());
                completed = true;
            } catch (const std::bad_alloc&) {
                ++failed_allocations;
                CHECK(connection.strength() == 0.75);
                CHECK(connection.revision() == revision);
                CHECK(connection.experiences().size() == 48);
            }
            CHECK(limited.used() == baseline);
            if (completed) break;
        }
        CHECK(completed && failed_allocations > 0);
        CHECK(connection.strength() == 0.75 * 1.01);
        CHECK(connection.revision() == revision + 1);
        std::printf("Refinement failure points checked: %u\n", failed_allocations);
    }
    {
        Connection overflowing(id(11), std::numeric_limits<double>::max(), rules, memory);
        originals.fill(overflowing, EvidenceOutcome::support);
        const auto before = overflowing.revision();
        const auto report = overflowing.refine(1, 5);
        CHECK(!report.result().strength().valid());
        CHECK(overflowing.strength() == std::numeric_limits<double>::max());
        CHECK(overflowing.revision() == before);
    }
    CHECK(memory.used() == 0);
    std::printf("PASS: %u connection checks\n", checks);
}
