// OpenGL include: the loader on Windows, system headers elsewhere.
#pragma once
#ifdef _WIN32
#include "glad.h"
#else
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#endif
