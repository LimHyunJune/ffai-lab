#pragma once

// C++ 링케이지로 선언 (구현도 C++ 파일이므로 extern "C" 불필요)
struct PersonSegORT;

PersonSegORT* personseg_create(const char* model_path, int in_w, int in_h, int num_threads);

int personseg_run(PersonSegORT* h,
                  const float* nchw, int n, int c, int hh, int ww,
                  float* out_mask /* size = hh*ww */);

void personseg_destroy(PersonSegORT** ph);
