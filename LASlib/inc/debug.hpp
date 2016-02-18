
/*
 * debug.hpp
 *
 *  Created on: , 02182016
 *      Author: jwendel@usgs.gov
 */
// example
// dbg(5,"test object class %s and declared name %s", typeid(*lasreader).name(), quote(*lasreader));
// DEBUG and DEBUG_LEVEL are passed in with makefile variables
// higher debug levels increase verbosity


#ifndef DEBUG_HPP_
#define DEBUG_HPP_


#ifdef DEBUG
#include <stdio.h>
#include <typeinfo>

#define dbg(level,fmt, ...) \
        do { if (DEBUG_LEVEL>=level) fprintf(stderr, "%s:%d:%s(): " fmt"\n", __FILE__, \
                                __LINE__, __func__, __VA_ARGS__); } while (0)

#define quote(x) #x



#else

#define dbg(level,fmt, ...) do{} while (0)
#define quote(x) ""

#endif


#endif /* DEBUG_HPP_ */
