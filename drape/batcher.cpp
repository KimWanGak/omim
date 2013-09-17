#include "batcher.hpp"
#include "../base/assert.hpp"

#include "../std/utility.hpp"

namespace
{
  struct BaseStrategy
  {
  public:
    BaseStrategy()
      : m_startIndex(0)
      , m_counter(0)
    {
    }

    void SetStartIndex(uint16_t startIndex)
    {
      m_startIndex = startIndex;
    }

  protected:
    uint16_t GetCounter()
    {
      return m_counter++;
    }

    uint16_t m_startIndex;
    uint16_t m_counter;
  };

  struct TrianglesListStrategy : public BaseStrategy
  {
  public:
    uint16_t GetIndexCount(uint16_t vertexCount)
    {
      return vertexCount;
    }

    uint16_t GetVertexCount(uint16_t indexCount)
    {
      return indexCount;
    }

    uint16_t operator()()
    {
      return m_startIndex + GetCounter();
    }
  };

  struct TrianglesStripStrategy : public BaseStrategy
  {
  public:
    uint16_t GetIndexCount(uint16_t vertexCount)
    {
      return 3* (vertexCount - 2);
    }

    uint16_t GetVertexCount(uint16_t indexCount)
    {
      return (indexCount / 3) + 2;
    }

    uint16_t operator()()
    {
      uint16_t counter = GetCounter();
      return m_startIndex + counter - 2 * (counter / 3);
    }
  };

  struct TrianglesFanStrategy : public BaseStrategy
  {
  public:
    uint16_t GetIndexCount(uint16_t vertexCount)
    {
      return 3* (vertexCount - 2);
    }

    uint16_t GetVertexCount(uint16_t indexCount)
    {
      return (indexCount / 3) + 2;
    }

    uint16_t operator()()
    {
      uint16_t counter = GetCounter();
      if ((counter % 3) == 0)
        return m_startIndex;
      return m_startIndex + counter - 2 * (counter / 3);
    }
  };
}

Batcher::Batcher(WeakPointer<IBatchFlush> flushInterface)
  : m_flushInterface(flushInterface)
{
}

Batcher::~Batcher()
{
  buckets_t::iterator it = m_buckets.begin();
  for (; it != m_buckets.end(); ++it)
    it->second.Destroy();
}

template <typename strategy>
void Batcher::InsertTriangles(const GLState & state, strategy s, WeakPointer<AttributeProvider> params)
{
  while (params->IsDataExists())
  {
    uint16_t vertexCount = params->GetVertexCount();
    uint16_t indexCount = s.GetIndexCount(vertexCount);

    WeakPointer<VertexArrayBuffer> buffer = GetBuffer(state);
    uint16_t availableVertexCount = buffer->GetAvailableVertexCount();
    uint16_t availableIndexCount = buffer->GetAvailableIndexCount();

    ASSERT(availableIndexCount != 0, ("Buffer must be filnalized on previous iteration"));
    ASSERT(availableVertexCount != 0, ("Buffer must be filnalized on previous iteration"));

    bool needFinalizeBuffer = false;
    if (vertexCount > availableVertexCount || indexCount > availableIndexCount)
    {
      needFinalizeBuffer = true;
      if (s.GetIndexCount(availableVertexCount) <= availableIndexCount)
        vertexCount = availableVertexCount;
      else
        vertexCount = s.GetVertexCount(availableIndexCount);

      indexCount = s.GetIndexCount(vertexCount);
    }

    /// generate indexes
    uint16_t startIndexValue = buffer->GetStartIndexValue();
    s.SetStartIndex(startIndexValue);
    vector<uint16_t> indexes;
    indexes.resize(indexCount);
    std::generate(indexes.begin(), indexes.end(), s);

    buffer->UploadIndexes(&indexes[0], indexCount);

    /// upload data from params to GPU buffers
    for (size_t i = 0; i < params->GetStreamCount(); ++i)
    {
      WeakPointer<GLBuffer> streamBuffer = buffer->GetBuffer(params->GetBindingInfo(i));
      streamBuffer->UploadData(params->GetRawPointer(i), vertexCount);
    }

    params->Advance(vertexCount);
    if (needFinalizeBuffer)
      FinalizeBuffer(state);
  }
}

void Batcher::InsertTriangleList(const GLState & state, WeakPointer<AttributeProvider> params)
{
  InsertTriangles(state, TrianglesListStrategy(), params);
}

void Batcher::InsertTriangleStrip(const GLState & state, WeakPointer<AttributeProvider> params)
{
  InsertTriangles(state, TrianglesStripStrategy(), params);
}

void Batcher::InsertTriangleFan(const GLState & state, WeakPointer<AttributeProvider> params)
{
  InsertTriangles(state, TrianglesFanStrategy(), params);
}

void Batcher::RequestIncompleteBuckets()
{
  for (buckets_t::iterator it = m_buckets.begin(); it != m_buckets.end(); ++it)
    m_flushInterface->UseIncompleteBucket(it->first, it->second.GetWeakPointer());
}

WeakPointer<VertexArrayBuffer> Batcher::GetBuffer(const GLState & state)
{
  buckets_t::iterator it = m_buckets.find(state);
  if (it != m_buckets.end())
    return it->second.GetWeakPointer();

  StrongPointer<VertexArrayBuffer> buffer(new VertexArrayBuffer(768, 512));
  m_buckets.insert(make_pair(state, buffer));
  return buffer.GetWeakPointer();
}

void Batcher::FinalizeBuffer(const GLState & state)
{
  ASSERT(m_buckets.find(state) != m_buckets.end(), ("Have no bucket for finalize with given state"));
  m_flushInterface->FlushFullBucket(state, m_buckets[state]);
  m_buckets.erase(state);
}
