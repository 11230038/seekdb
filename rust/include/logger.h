#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdollar-in-identifier-extension"
#endif // __clang__

namespace rust {
inline namespace cxxbridge1 {
// #include "rust/cxx.h"

#ifndef CXXBRIDGE1_RUST_OPAQUE
#define CXXBRIDGE1_RUST_OPAQUE
class Opaque {
public:
  Opaque() = delete;
  Opaque(const Opaque &) = delete;
  ~Opaque() = delete;
};
#endif // CXXBRIDGE1_RUST_OPAQUE

#ifndef CXXBRIDGE1_IS_COMPLETE
#define CXXBRIDGE1_IS_COMPLETE
namespace detail {
namespace {
template <typename T, typename = std::size_t>
struct is_complete : std::false_type {};
template <typename T>
struct is_complete<T, decltype(sizeof(T))> : std::true_type {};
} // namespace
} // namespace detail
#endif // CXXBRIDGE1_IS_COMPLETE

#ifndef CXXBRIDGE1_LAYOUT
#define CXXBRIDGE1_LAYOUT
class layout {
  template <typename T>
  friend std::size_t size_of();
  template <typename T>
  friend std::size_t align_of();
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return T::layout::size();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_size_of() {
    return sizeof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      size_of() {
    return do_size_of<T>();
  }
  template <typename T>
  static typename std::enable_if<std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return T::layout::align();
  }
  template <typename T>
  static typename std::enable_if<!std::is_base_of<Opaque, T>::value,
                                 std::size_t>::type
  do_align_of() {
    return alignof(T);
  }
  template <typename T>
  static
      typename std::enable_if<detail::is_complete<T>::value, std::size_t>::type
      align_of() {
    return do_align_of<T>();
  }
};

template <typename T>
std::size_t size_of() {
  return layout::size_of<T>();
}

template <typename T>
std::size_t align_of() {
  return layout::align_of<T>();
}
#endif // CXXBRIDGE1_LAYOUT
} // namespace cxxbridge1
} // namespace rust

#if __cplusplus >= 201402L
#define CXX_DEFAULT_VALUE(value) = value
#else
#define CXX_DEFAULT_VALUE(value)
#endif

namespace oceanbase {
  namespace logger {
    struct RecordMetadata;
    struct RecordBuffer;
    enum class OptionKey : ::std::uint8_t;
    enum class ContextKey : ::std::uint8_t;
    enum class RecordFlag : ::std::uint32_t;
    struct LevelMap;
  }
}

namespace oceanbase {
namespace logger {
#ifndef CXXBRIDGE1_STRUCT_oceanbase$logger$RecordMetadata
#define CXXBRIDGE1_STRUCT_oceanbase$logger$RecordMetadata
struct RecordMetadata final {
  char const *module CXX_DEFAULT_VALUE(nullptr);
  char const *file CXX_DEFAULT_VALUE(nullptr);
  char const *function CXX_DEFAULT_VALUE(nullptr);
  char const *event CXX_DEFAULT_VALUE(nullptr);
  char const *thread_name CXX_DEFAULT_VALUE(nullptr);
  ::std::int64_t timestamp_us CXX_DEFAULT_VALUE(0);
  ::std::int64_t thread_id CXX_DEFAULT_VALUE(0);
  ::std::int32_t line CXX_DEFAULT_VALUE(0);
  ::std::int32_t level CXX_DEFAULT_VALUE(0);
  ::std::int32_t error CXX_DEFAULT_VALUE(0);
  ::std::int32_t with_head CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$logger$RecordMetadata

#ifndef CXXBRIDGE1_STRUCT_oceanbase$logger$RecordBuffer
#define CXXBRIDGE1_STRUCT_oceanbase$logger$RecordBuffer
struct RecordBuffer final {
  ::std::uint8_t *record CXX_DEFAULT_VALUE(nullptr);
  char *data CXX_DEFAULT_VALUE(nullptr);
  ::std::size_t capacity CXX_DEFAULT_VALUE(0);
  ::std::uint32_t flags CXX_DEFAULT_VALUE(0);

  using IsRelocatable = ::std::true_type;
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$logger$RecordBuffer

#ifndef CXXBRIDGE1_ENUM_oceanbase$logger$OptionKey
#define CXXBRIDGE1_ENUM_oceanbase$logger$OptionKey
enum class OptionKey : ::std::uint8_t {
  Async = 0,
  StopAppend = 1,
  Perf = 2,
  IgnoreRequestLevel = 3,
  ForceCheck = 4,
  InfoAsWdiag = 5,
  Stopped = 6,
  Initialized = 7,
  AlertLevel = 8,
};
#endif // CXXBRIDGE1_ENUM_oceanbase$logger$OptionKey

#ifndef CXXBRIDGE1_ENUM_oceanbase$logger$ContextKey
#define CXXBRIDGE1_ENUM_oceanbase$logger$ContextKey
enum class ContextKey : ::std::uint8_t {
  Level = 0,
  Disabled = 1,
  Guard = 2,
  TraceMode = 3,
  LastCost = 4,
};
#endif // CXXBRIDGE1_ENUM_oceanbase$logger$ContextKey

#ifndef CXXBRIDGE1_ENUM_oceanbase$logger$RecordFlag
#define CXXBRIDGE1_ENUM_oceanbase$logger$RecordFlag
enum class RecordFlag : ::std::uint32_t {
  Backtrace = 1,
};
#endif // CXXBRIDGE1_ENUM_oceanbase$logger$RecordFlag

#ifndef CXXBRIDGE1_STRUCT_oceanbase$logger$LevelMap
#define CXXBRIDGE1_STRUCT_oceanbase$logger$LevelMap
struct LevelMap final : public ::rust::Opaque {
  ~LevelMap() = delete;

private:
  friend ::rust::layout;
  struct layout {
    static ::std::size_t size() noexcept;
    static ::std::size_t align() noexcept;
  };
};
#endif // CXXBRIDGE1_STRUCT_oceanbase$logger$LevelMap

::std::int32_t flush() noexcept;

::std::int32_t shutdown() noexcept;

::std::int32_t set_new_file_info(char const *info) noexcept;

::std::int32_t check_files() noexcept;

::std::int32_t set_file(::std::size_t route, char const *path, bool redirect) noexcept;

::oceanbase::logger::LevelMap *map_clone(::oceanbase::logger::LevelMap const *map) noexcept;

void map_free(::oceanbase::logger::LevelMap *map) noexcept;

::std::int32_t map_level(::oceanbase::logger::LevelMap const *map, ::std::int32_t parent, ::std::int32_t sub) noexcept;

::std::int32_t map_set(::oceanbase::logger::LevelMap **map, ::std::int32_t parent, ::std::int32_t sub, ::std::int32_t level) noexcept;

void context_map(::oceanbase::logger::LevelMap const *map) noexcept;

::std::int64_t context_value(::oceanbase::logger::ContextKey key, ::std::int64_t value, ::std::int32_t write) noexcept;

::std::int64_t option(::oceanbase::logger::OptionKey key, ::std::int64_t value, ::std::int32_t write) noexcept;

::std::int32_t get_level(::std::int32_t parent, ::std::int32_t sub, ::std::int32_t request) noexcept;

::std::int32_t dba_enabled(::std::int32_t level, ::std::int32_t force) noexcept;

::std::int32_t set_instance_level(::std::int32_t level, ::std::int64_t version) noexcept;

::std::int32_t register_module(::std::int32_t parent, ::std::int32_t sub, char const *name) noexcept;

::std::int32_t find_module(char const *name, char const *sub_name, ::std::uint64_t *parent, ::std::uint64_t *sub) noexcept;

::std::int32_t parse_level(char const *name, ::std::int8_t *level, ::std::int32_t alert) noexcept;

::std::int32_t parse_settings(char const *text, ::std::int32_t len, ::std::int32_t *valid, ::oceanbase::logger::LevelMap **map, ::std::int64_t version, ::std::int32_t check) noexcept;

char const *level_name(::std::int32_t level) noexcept;

::std::uint64_t const *trace_id() noexcept;

void set_trace_id(::std::uint64_t const *words) noexcept;

::std::uint64_t trace_sequence() noexcept;

void trace_seed(::std::uint64_t seed) noexcept;

void trace_field(::std::size_t field, ::std::uint64_t value) noexcept;

::std::int32_t begin(::oceanbase::logger::RecordMetadata const *meta, ::oceanbase::logger::RecordBuffer *out) noexcept;

::std::int32_t grow(::oceanbase::logger::RecordBuffer *buffer) noexcept;

::std::int32_t commit(::oceanbase::logger::RecordBuffer *buffer, ::std::size_t body_len, char const *backtrace) noexcept;

void cancel(::oceanbase::logger::RecordBuffer *buffer) noexcept;

::std::int32_t trace_flush(char const *module, ::std::int32_t level) noexcept;

::std::int32_t start() noexcept;

::std::uint64_t file_limit(::std::size_t key, ::std::uint64_t value, ::std::int32_t write) noexcept;
} // namespace logger
} // namespace oceanbase

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
