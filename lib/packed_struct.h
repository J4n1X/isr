#ifndef PACKED_STRUCT_MACROS_H
#define PACKED_STRUCT_MACROS_H

#if defined(_MSC_VER)
    #define PACK_STRUCT_BEGIN __pragma(pack(push, 1))
    #define PACK_STRUCT_END   __pragma(pack(pop))
    #define PACK_STRUCT_ATTR
#elif defined(__GNUC__) || defined(__clang__)
    #define PACK_STRUCT_BEGIN
    #define PACK_STRUCT_END
    #define PACK_STRUCT_ATTR __attribute__((__packed__))
#else
    #error "Unknown compiler. Cannot define cross-platform packed structs."
#endif

#endif /* PACKED_STRUCT_MACROS_H */