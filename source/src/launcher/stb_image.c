/*
 * stb_image.c - the stb_image implementation (public domain / MIT), PNG
 * only, for the launcher background.
 */
#pragma clang diagnostic ignored "-Weverything"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
