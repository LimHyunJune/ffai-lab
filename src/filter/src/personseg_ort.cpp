#include "personseg_ort.h"
#include <libonnxruntime/onnxruntime_cxx_api.h>

#include <vector>
#include <string>
#include <memory>
#include <array>
#include <algorithm>
#include <cstdio>      // std::fprintf
#include <cstring>     // std::memcpy
#include <sys/stat.h>  // ::stat

struct PersonSegORT {
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "personseg"};
  Ort::SessionOptions opts;
  std::unique_ptr<Ort::Session> session;
  std::vector<std::string> inNames, outNames;
  int W=0, H=0;

  PersonSegORT(const char* model_path, int w, int h, int threads): W(w), H(h) {
    // 1) 모델 파일 체크
    if (!model_path || !*model_path) {
      std::fprintf(stderr, "[personseg_ort] init error: model path is null/empty\n");
      throw std::runtime_error("model path empty");
    }
    struct stat st{};
    if (::stat(model_path, &st) != 0) {
      std::fprintf(stderr, "[personseg_ort] init error: model not found: %s\n", model_path);
      throw std::runtime_error("model not found");
    }

    // 2) 세션 옵션
    if (threads > 0) opts.SetIntraOpNumThreads(threads);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);

    // ⚠️ CPU EP는 기본값이므로 명시 호출 불필요 (일부 빌드엔 심볼 없음)
    // OrtSessionOptionsAppendExecutionProvider_CPU 호출 제거

    // 3) 세션 생성
    try {
      std::fprintf(stderr, "[personseg_ort] creating session: %s (W=%d, H=%d, threads=%d)\n",
                   model_path, W, H, threads);
      session = std::make_unique<Ort::Session>(env, model_path, opts);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[personseg_ort] init error: Ort::Session ctor failed: %s\n", e.what());
      throw;
    } catch (...) {
      std::fprintf(stderr, "[personseg_ort] init error: Ort::Session ctor failed: unknown\n");
      throw;
    }

    // 4) IO 이름 확보
    Ort::AllocatorWithDefaultOptions alloc;
    size_t num_in  = 0;
    size_t num_out = 0;
    try {
      num_in  = session->GetInputCount();
      num_out = session->GetOutputCount();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[personseg_ort] init error: GetInput/OutputCount failed: %s\n", e.what());
      throw;
    }
    if (num_in == 0 || num_out == 0) {
      std::fprintf(stderr, "[personseg_ort] init error: invalid IO counts (in=%zu out=%zu)\n", num_in, num_out);
      throw std::runtime_error("invalid model io");
    }

    try {
      { auto n = session->GetInputNameAllocated(0, alloc);  inNames.emplace_back(n.get()); }
      { auto n = session->GetOutputNameAllocated(0, alloc); outNames.emplace_back(n.get()); }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[personseg_ort] init error: GetInput/OutputName failed: %s\n", e.what());
      throw;
    }

    std::fprintf(stderr, "[personseg_ort] session ready. in=%s out=%s\n",
                 inNames.empty()  ? "(none)" : inNames[0].c_str(),
                 outNames.empty() ? "(none)" : outNames[0].c_str());
  }
};

PersonSegORT* personseg_create(const char* model_path, int in_w, int in_h, int num_threads){
  try {
    return new PersonSegORT(model_path, in_w, in_h, num_threads);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[personseg_ort] init error: %s\n", e.what());
    return nullptr;
  } catch (...) {
    std::fprintf(stderr, "[personseg_ort] init error: unknown\n");
    return nullptr;
  }
}

int personseg_run(PersonSegORT* h,
                  const float* nchw, int n, int c, int hh, int ww,
                  float* out_mask)
{
  if (!h || !h->session || !nchw || !out_mask) {
    std::fprintf(stderr, "[personseg_ort] run error: invalid args (h=%p, in=%p, out=%p)\n",
                 (void*)h, (const void*)nchw, (void*)out_mask);
    return -1;
  }

  try {
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtDeviceAllocator, OrtMemTypeCPU);

    std::array<int64_t,4> ishape{n, c, hh, ww};
    const size_t icount = (size_t)n * c * hh * ww;

    Ort::Value in = Ort::Value::CreateTensor<float>(
        mem, const_cast<float*>(nchw), icount, ishape.data(), ishape.size()
    );

    auto outs = h->session->Run(Ort::RunOptions{nullptr},
                                (const char* const*)h->inNames.data(), &in, 1,
                                (const char* const*)h->outNames.data(), 1);
    if (outs.empty()) {
      std::fprintf(stderr, "[personseg_ort] run error: no outputs\n");
      return -2;
    }

    auto& o = outs.front();
    auto info = o.GetTensorTypeAndShapeInfo();
    auto shp  = info.GetShape();
    if (shp.size() < 2) {
      std::fprintf(stderr, "[personseg_ort] run error: invalid output rank (rank=%zu)\n", shp.size());
      return -3;
    }
    const int oh = (int)shp[shp.size()-2];
    const int ow = (int)shp[shp.size()-1];
    const size_t ocount = (size_t)oh * ow;

    const float* ptr = nullptr;
    try {
      ptr = o.GetTensorData<float>();
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[personseg_ort] run error: GetTensorData<float> failed: %s\n", e.what());
      return -4;
    }

    if (!ptr) {
      std::fprintf(stderr, "[personseg_ort] run error: output data null\n");
      return -5;
    }

    std::memcpy(out_mask, ptr, ocount * sizeof(float));
    return (int)ocount;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[personseg_ort] run error: %s\n", e.what());
    return -100;
  } catch (...) {
    std::fprintf(stderr, "[personseg_ort] run error: unknown\n");
    return -101;
  }
}

void personseg_destroy(PersonSegORT** ph){
  if (ph && *ph) { delete *ph; *ph = nullptr; }
}
