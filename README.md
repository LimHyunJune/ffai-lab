- FFMPEG (streaming pipeline, custome filter) <br>
https://trac.ffmpeg.org/wiki/CompilationGuide/Ubuntu

- ONNX (AI model integration) <br>
https://github.com/microsoft/onnxruntime/releases
```
VER=1.23.1
wget -O onnxruntime-linux-x64-$VER.tgz \
  https://github.com/microsoft/onnxruntime/releases/download/v$VER/onnxruntime-linux-x64-$VER.tgz
tar -xzf onnxruntime-linux-x64-$VER.tgz
```

- Boost log
```
sudo apt install -y libboost-dev libboost-log-dev
```
