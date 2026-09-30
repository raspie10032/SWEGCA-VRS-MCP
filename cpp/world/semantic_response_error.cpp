#include "world/semantic_response_error.hpp"

#include "world/semantic_encoding.hpp"

#include <set>
#include <utility>

namespace swegca::world {

SemanticResponseError::SemanticResponseError(
    std::string code, std::optional<std::string> content,
    std::optional<std::string> reason)
    : InterfaceError(code), failure_code(std::move(code)),
      finish_reason([&] {
          static const std::set<std::string, std::less<>> known{
              "stop", "length", "tool_calls", "function_call", "content_filter"};
          if (!reason) return std::string("missing");
          return known.contains(*reason) ? *reason : std::string("other");
      }()),
      response_utf8(content && content->size() <= semantic_response_max_bytes
                        ? std::move(content) : std::nullopt) {}

}  // namespace swegca::world
