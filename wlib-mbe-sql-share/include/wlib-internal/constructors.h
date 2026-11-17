/*
 * Small include that disables constructor priorities if gcc does not support it.
 * It is important to have the compiler honor this attribute when compiling production code,
 * but gcc on some platforms (e.g. Darwin) does not implement this feature.
 *
 * To disable the constructor, define "DISABLE_CONSTRUCTOR_PRIORITES" at cmake level
 * (e.g. /usr/local/etc/cmake/mdef1.cmake )
 */

#ifndef __constructors_h

#define __constructors_h

#define HK_CONSTRUCTOR_INIT       101
#define HK_CONSTRUCTOR_REGISTER   102
#define HK_CONSTRUCTOR_END        103

#if defined(DISABLE_CONSTRUCTOR_PRIORITES)
#define constructor(level) constructor
#endif

#endif