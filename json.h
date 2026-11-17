#pragma once
#include <stdint.h>



typedef enum {
    JSON_limbInt,
    JSON_limbFloat,
    JSON_limbDouble,
    JSON_String,
    JSON_dict,
    JSON_array, // list
    JSON_bool,
    JSON_null,
} json_type;

typedef struct {
    char* key;
    json_type type;
    union {
        int64_t i;
        float f;
        double d;
        char* s;
        struct {
            struct json_pair* items;
            size_t count, cap;
        } dict; // key-value pairs
        struct {
            struct json_pair* items;
            size_t count, cap;
        } array; // list of values
        int b; // boolean
        void* n; // null
    } value;
} json_data;

typedef struct {
    char* key;
    char* value_hex;
    json_type type;
    char* (*get_value)(const char* key);
} json_pair;

// void parse_value(char* value, json_type type);