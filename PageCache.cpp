#include "PageCache.h"

PageCache PageCache::_inst;


Span* PageCache::AllocBigPageObj(size_t size)
{
	assert(size > MAX_BYTES);

	size = SizeClass::_Roundup(size, 12);
	size_t npage =  size >> PAGE_SHIFT;	//以内存的地址右移12为页号

	//如果小于等于128页
	if (npage < NPAGES)
	{
		Span* span = NewSpan(npage);
		span->_objsize = size;
		return span;
	}
	else
	{
		//大于128页调用系统调用
		void* ptr = VirtualAlloc(0, npage << PAGE_SHIFT,
			MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (ptr == nullptr)
			throw std::bad_alloc();

		Span* span = new Span;
		span->_npage = npage;
		span->_pageid = (PageID)ptr >> PAGE_SHIFT;
		span->_objsize = npage << PAGE_SHIFT;

		_idspanmap[span->_pageid] = span;

		return span;
	}
}

void PageCache::FreeBigPageObj(void* ptr, Span* span)
{
	size_t npage = span->_objsize >> PAGE_SHIFT;
	if (npage < NPAGES)
	{
		span->_objsize = 0;
		ReleaseSpanToPageCahce(span);
	}
	else
	{
		_idspanmap.erase(npage);
		delete span;
		VirtualFree(ptr, 0, MEM_RELEASE);
	}
}

Span* PageCache::NewSpan(size_t n)
{
	std::unique_lock<std::mutex> lock(_mtx);
	return _NewSpan(n);
}

//页缓存在已有内存里分割一个新span
Span* PageCache::_NewSpan(size_t n)
{
	assert(n < NPAGES);

	//如果对应大小的spanlist不为空就，直接pop，并返回
	if (!_spanlist[n].Empty())
	{
		return _spanlist[n].PopFront();
	}

	//此时说明page Cache中没有n大小的span，需要分割
	//从n往后遍历
	for (size_t i = n+1; i < NPAGES; ++i)
	{
		//只要找到第一个不为空的spanlist即可
		if (!_spanlist[i].Empty())
		{
			Span* span = _spanlist[i].PopFront();
			Span* split = new Span;

			//pop出这个比较大的span，并切割
			split->_pageid = span->_pageid;
			split->_npage = n;
			span->_pageid = span->_pageid + n;
			span->_npage = span->_npage - n;

			for (size_t i = 0; i < n; ++i)
			{
				_idspanmap[split->_pageid + i] = split;
			}

			_spanlist[span->_npage].PushFront(span);
			return split;
		}
	}

	//此时说明在页缓存的往后的spanlist中也没有找到，所以调用系统调用
	Span* span = new Span;

#ifdef _WIN32
	void* ptr = VirtualAlloc(0, (NPAGES - 1)*(1 << PAGE_SHIFT),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
// brk
#endif

	//如果系统调用失败，抛异常
	if (ptr == nullptr)
	{
		throw std::bad_alloc();
	}

	span->_pageid = (PageID)ptr >> PAGE_SHIFT;
	span->_npage = NPAGES - 1;

	for (size_t i = 0; i < span->_npage; ++i)
	{
		_idspanmap[span->_pageid+i] = span;
	}

	//将新申请的span插入
	_spanlist[span->_npage].PushFront(span);
	return _NewSpan(n);
}

// 获取从对象到span的映射：某个对象是哪个span的
Span* PageCache::MapObjectToSpan(void* obj)
{
	PageID id = (PageID)obj >> PAGE_SHIFT;
	auto it = _idspanmap.find(id);
	if (it != _idspanmap.end())
	{
		return it->second;
	}
	else
	{
		assert(false);
		return nullptr;
	}
}

//将span释放给页缓存
void PageCache::ReleaseSpanToPageCahce(Span* cur)
{
	std::unique_lock<std::mutex> lock(_mtx);

	// 向前合并
	while (1)
	{
		PageID curid = cur->_pageid;
		PageID previd = curid - 1;
		auto it = _idspanmap.find(previd);

		// 没有找到
		if (it == _idspanmap.end())
			break;

		// 前一个span不空闲
		if (it->second->_usecount != 0)
			break;

		Span* prev = it->second;

		// 超过128页，则不合并
		if (cur->_npage + prev->_npage > NPAGES - 1)
			break;


		// 先把prev从链表中移除
		_spanlist[prev->_npage].Erase(prev);

		// 合并
		prev->_npage += cur->_npage;
		// 修正id->span的映射关系
		for (PageID i = 0; i < cur->_npage; ++i)
		{
			_idspanmap[cur->_pageid + i] = prev;
		}
		delete cur;

		// 继续向前合并
		cur = prev;
	}

	// 向后合并
	while (1)
	{
		PageID curid = cur->_pageid;
		PageID nextid = curid + cur->_npage;
		auto it = _idspanmap.find(nextid);
		if (it == _idspanmap.end())
			break;

		if (it->second->_usecount != 0)
			break;

		// 合并
		Span* next = it->second;
		// 超过128页，则不合并
		if (cur->_npage + next->_npage > NPAGES - 1)
			break;

		_spanlist[next->_npage].Erase(next);

		cur->_npage += next->_npage;
		// 修正id->span的映射关系
		for (size_t i = 0; i < next->_npage; ++i)
		{
			_idspanmap[next->_pageid + i] = cur;
		}
		delete next;
	}

	cur->_list = nullptr;
	cur->_objsize = 0;
	_spanlist[cur->_npage].PushFront(cur);
}