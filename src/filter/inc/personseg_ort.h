#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/* opaque ORT 세션 핸들 */
typedef struct PersonSegORT PersonSegORT;

/* 세션 생성: model_path(onnx), 모델 입력크기(in_w,in_h), intra-op 스레드 수 */
PersonSegORT* personseg_create(const char* model_path, int in_w, int in_h, int num_threads);

/* 추론: 입력 NCHW(float, [0..1]) → 출력 마스크(HxW, float[0..1]), 반환값=out 요소 수(H*W) 또는 음수(에러) */
int personseg_run(PersonSegORT* h,
                  const float* nchw, int n, int c, int hgt, int wid,
                  float* out_mask /* size = hgt*wid */);

/* 세션 파괴 */
void personseg_destroy(PersonSegORT** ph);

#ifdef __cplusplus
}
#endif
