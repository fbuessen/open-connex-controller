#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace firmware {
// Bound ArduinoJson 7's otherwise growable heap allocation per request/response.
class JsonAllocator : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Header { size_t size; };
  size_t used_ = 0;
 public:
  void* allocate(size_t size) override {
    if (size > 8192 - used_) return nullptr;
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size; used_ += size; return h + 1;
  }
  void deallocate(void* pointer) override {
    if (!pointer) return;
    auto* h = static_cast<Header*>(pointer) - 1;
    used_ -= h->size; std::free(h);
  }
  void* reallocate(void* pointer, size_t size) override {
    if (!pointer) return allocate(size);
    auto* h = static_cast<Header*>(pointer) - 1;
    const size_t old = h->size;
    if (size > 8192 - used_ + old) return nullptr;
    h = static_cast<Header*>(std::realloc(h, sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size; used_ = used_ - old + size; return h + 1;
  }
};
struct Json {
  JsonAllocator allocator;
  JsonDocument value{&allocator};
};
inline bool parseJson(JsonDocument& output, const char* data, size_t size) {
  if (size > 1024) return false;
  struct Reader {
    const char* data;
    size_t left;
    int read() { if (!left) return -1; --left; return static_cast<unsigned char>(*data++); }
    size_t readBytes(char* out, size_t count) {
      if (count > left) count = left;
      memcpy(out, data, count); data += count; left -= count; return count;
    }
  } reader{data, size};
  const auto error = deserializeJson(output, reader, DeserializationOption::NestingLimit(3));
  if (error || !output.is<JsonObject>()) return false;
  for (int c = reader.read(); c >= 0; c = reader.read())
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
  return true;
}
}
