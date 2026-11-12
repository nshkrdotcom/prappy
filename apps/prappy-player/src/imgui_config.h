#pragma once
namespace reporting { [[noreturn]] void assertion(const char*,const char*,int) noexcept; }
// Keep programmer assertions active in shipped builds as well as Debug.
#define IM_ASSERT(expression) do { if(!(expression)) reporting::assertion(#expression,__FILE__,__LINE__); } while(0)
