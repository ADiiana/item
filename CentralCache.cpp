#include "CentralCache.h"
#include "PageCache.h"

//����ȫ�ֱ���������ģʽ
CentralCache CentralCache::_inst;

//�����Ļ���Ķ�Ӧspanlist�л�ȡһ��span
Span* CentralCache::GetOneSpan(SpanList& spanlist, size_t byte_size)
{
	Span* span = spanlist.Begin();
	while (span != spanlist.End())
	{
		if (span->_list != nullptr)
			return span;
		else
			span = span->_next;
	}

	//���Ļ�����û�б���ϵͳ����
	Span* newspan = PageCache::GetInstance()->NewSpan(SizeClass::NumMovePage(byte_size));
	// ��spanҳ�зֳ���Ҫ�Ķ�����������
	char* cur = (char*)(newspan->_pageid << PAGE_SHIFT);
	char* end = cur + (newspan->_npage << PAGE_SHIFT);
	newspan->_list = cur;
	newspan->_objsize = byte_size;

	while (cur + byte_size < end)
	{
		char* next = cur + byte_size;
		NEXT_OBJ(cur) = next;
		cur = next;
	}
	NEXT_OBJ(cur) = nullptr;

	spanlist.PushFront(newspan);
	return newspan;
}

//�����ܴ����Ļ���ĸ���spanlist�л�ȡ���ٵĶ���
size_t CentralCache::FetchRangeObj(void*& start, void*& end, size_t n, size_t byte_size)
{
	//�ҵ���Ӧ��spanlist��
	size_t index = SizeClass::Index(byte_size);
	SpanList& spanlist = _spanlist[index];

	// ����
	//spanlist.Lock();
	std::unique_lock<std::mutex> lock(spanlist._mtx);

	Span* span = GetOneSpan(spanlist, byte_size);

	// ��span�л�ȡrange����
	size_t batchsize = 0;
	void* prev = nullptr;
	void* cur = span->_list;

	//��n�����󶼷����Thread Cache
	for (size_t i = 0; i < n; ++i)
	{
		prev = cur;
		cur = NEXT_OBJ(cur);
		++batchsize;
		if (cur == nullptr)
			break;
	}

	start = span->_list;
	end = prev;

	span->_list = cur;
	span->_usecount += batchsize;

	// ���յ�span�Ƶ���󣬱��ַǿ�span��ǰ�档
	if (span->_list == nullptr)
	{
		spanlist.Erase(span);
		spanlist.PushBack(span);
	}

	//spanlist.Unlock();

	return batchsize;
}

//�ͷŶ����span
void CentralCache::ReleaseListToSpans(void* start, size_t size)
{
	//
	size_t index = SizeClass::Index(size);
	SpanList& spanlist = _spanlist[index];
	std::unique_lock<std::mutex> lock(spanlist._mtx);

	//������һ��һ���ͷ�
	while (start)
	{
		void* next = NEXT_OBJ(start);
		// ����
		//spanlist.Lock();
		//std::unique_lock<std::mutex> lock(spanlist._mtx);

		Span* span = PageCache::GetInstance()->MapObjectToSpan(start);
		NEXT_OBJ(start) = span->_list;
		span->_list = start;
		// ��һ��span�Ķ���ȫ���ͷŻ�������span����pagecache��������ҳ�ϲ�
		if (--span->_usecount == 0)
		{
			spanlist.Erase(span);
			PageCache::GetInstance()->ReleaseSpanToPageCahce(span);
		}

		//spanlist.Unlock();

		start = next;
	}
}

