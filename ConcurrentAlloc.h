#pragma once
#include "Common.h"
#include "ThreadCache.h"
#include "PageCache.h"

static inline void* ConcurrentAlloc(size_t size)
{
	if (size > MAX_BYTES)
	{
		//return malloc(size);
		//如果大于64KB，直接向页缓存申请
		Span* span = PageCache::GetInstance()->AllocBigPageObj(size);

		//由页号获得页的地址
		void* ptr = (void*)(span->_pageid << PAGE_SHIFT);
		return ptr;
	}
	else
	{
		if (tlslist == nullptr)
		{
			//cout << std::this_thread::get_id() << endl;
			tlslist = new ThreadCache;
			//cout << tlslist << endl;
		}

		//cout << tlslist << endl;

		return tlslist->Allocate(size);
	}
}

static inline void ConcurrentFree(void* ptr)
{
	Span* span = PageCache::GetInstance()->MapObjectToSpan(ptr);
	size_t size = span->_objsize;
	if (size > MAX_BYTES)
	{
		PageCache::GetInstance()->FreeBigPageObj(ptr, span);
	}
	else
	{
		tlslist->Deallocate(ptr, size);
	}
}