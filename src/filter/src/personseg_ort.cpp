#include "personseg_ort.h"
#include <libonnxruntime/onnxruntime_cxx_api.h>
#include <vector>
#include <string>
#include <memory>
#include <array>
#include <algorithm>

struct PersonSegORT {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "personseg"};
  Ort::SessionOptions opts;
  std::unique_ptr<Ort::Session> session;
  std::vector<std::string> inNames, outNames;
  int W=0, H=0;

  PersonSegORT(const char* model_path, int w, int h, int threads): W(w), H(h) {
    if (threads > 0) opts.SetIntraOpNumThreads(threads);
    session = std::make_unique<Ort::Session>(env, model_path, opts);
    Ort::AllocatorWithDefaultOptions alloc;
    { auto n = session->GetInputNameAllocated(0, alloc);  inNames.emplace_back(n.get()); }
    { auto n = session->GetOutputNameAllocated(0, alloc); outNames.emplace_back(n.get()); }
  }
};

PersonSegORT* personseg_create(const char* model_path, int in_w, int in_h, int num_threads){
  try { return new PersonSegORT(model_path, in_w, in_h, num_threads); }
  catch (...) { return nullptr; }
}

int personseg_run(PersonSegORT* h,
                  const float* nchw, int n, int c, int hh, int ww,
                  float* out_mask)
{
  if (!h || !h->session || !nchw || !out_mask) return -1;
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

  std::array<int64_t,4> ishape{n, c, hh, ww};
  const size_t icount = (size_t)n * c * hh * ww;
  Ort::Value in = Ort::Value::CreateTensor<float>(mem,
                    const_cast<float*>(nchw), icount, ishape.data(), ishape.size());

  auto outs = h->session->Run(Ort::RunOptions{nullptr},
                              (const char* const*)h->inNames.data(), &in, 1,
                              (const char* const*)h->outNames.data(), 1);

  if (outs.empty()) return -2;
  auto& o = outs.front();
  auto info = o.GetTensorTypeAndShapeInfo();
  auto shp  = info.GetShape();
  if (shp.size() < 2) return -3;
  const int oh = (int)shp[shp.size()-2];
  const int ow = (int)shp[shp.size()-1];
  const size_t ocount = (size_t)oh * ow;

  const float* ptr = o.GetTensorData<float>();
  std::copy(ptr, ptr + ocount, out_mask); // (1,1,oh,ow) 가정
  return (int)ocount;
}

void personseg_destroy(PersonSegORT** ph){
  if (ph && *ph) { delete *ph; *ph = nullptr; }
}
