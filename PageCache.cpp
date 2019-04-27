#include "PageCache.h"

PageCache PageCache::_inst;


Span* PageCache::AllocBigPageObj(size_t size)
{
	assert(size > MAX_BYTES);

	size = SizeClass::_Roundup(size, 12);
	size_t npage =  size >> PAGE_SHIFT;	//���ڴ�ĵ�ַ����12Ϊҳ��

	//���С�ڵ���128ҳ
	if (npage < NPAGES)
	{
		Span* span = NewSpan(npage);
		span->_objsize = size;
		return span;
	}
	else
	{
		//����128ҳ����ϵͳ����
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

//ҳ�����������ڴ���ָ�һ����span
Span* PageCache::_NewSpan(size_t n)
{
	assert(n < NPAGES);

	//�����Ӧ��С��spanlist��Ϊ�վͣ�ֱ��pop��������
	if (!_spanlist[n].Empty())
	{
		return _spanlist[n].PopFront();
	}

	//��ʱ˵��page Cache��û��n��С��span����Ҫ�ָ�
	//��n�������
	for (size_t i = n+1; i < NPAGES; ++i)
	{
		//ֻҪ�ҵ���һ����Ϊ�յ�spanlist����
		if (!_spanlist[i].Empty())
		{
			Span* span = _spanlist[i].PopFront();
			Span* split = new Span;

			//pop������Ƚϴ��span�����и�
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

	//��ʱ˵����ҳ����������spanlist��Ҳû���ҵ������Ե���ϵͳ����
	Span* span = new Span;

#ifdef _WIN32
	void* ptr = VirtualAlloc(0, (NPAGES - 1)*(1 << PAGE_SHIFT),
		MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
// brk
#endif

	//���ϵͳ����ʧ�ܣ����쳣
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

	//���������span����
	_spanlist[span->_npage].PushFront(span);
	return _NewSpan(n);
}

// ��ȡ�Ӷ���span��ӳ�䣺ĳ���������ĸ�span��
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

//��span�ͷŸ�ҳ����
void PageCache::ReleaseSpanToPageCahce(Span* cur)
{
	std::unique_lock<std::mutex> lock(_mtx);

	// ��ǰ�ϲ�
	while (1)
	{
		PageID curid = cur->_pageid;
		PageID previd = curid - 1;
		auto it = _idspanmap.find(previd);

		// û���ҵ�
		if (it == _idspanmap.end())
			break;

		// ǰһ��span������
		if (it->second->_usecount != 0)
			break;

		Span* prev = it->second;

		// ����128ҳ���򲻺ϲ�
		if (cur->_npage + prev->_npage > NPAGES - 1)
			break;


		// �Ȱ�prev���������Ƴ�
		_spanlist[prev->_npage].Erase(prev);

		// �ϲ�
		prev->_npage += cur->_npage;
		// ����id->span��ӳ���ϵ
		for (PageID i = 0; i < cur->_npage; ++i)
		{
			_idspanmap[cur->_pageid + i] = prev;
		}
		delete cur;

		// ������ǰ�ϲ�
		cur = prev;
	}

	// ���ϲ�
	while (1)
	{
		PageID curid = cur->_pageid;
		PageID nextid = curid + cur->_npage;
		auto it = _idspanmap.find(nextid);
		if (it == _idspanmap.end())
			break;

		if (it->second->_usecount != 0)
			break;

		// �ϲ�
		Span* next = it->second;
		// ����128ҳ���򲻺ϲ�
		if (cur->_npage + next->_npage > NPAGES - 1)
			break;

		_spanlist[next->_npage].Erase(next);

		cur->_npage += next->_npage;
		// ����id->span��ӳ���ϵ
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