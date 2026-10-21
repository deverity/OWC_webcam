# OWC — Old-phone WebCam

Transforma um celular Android em webcam de baixa latência para Windows, via USB
(adb) ou rede (TCP/UDP), com controle remoto total da câmera pelo PC, saída
como webcam virtual (DirectShow) e fonte nativa no OBS.

## Componentes

```
android/                      App Android (Gradle, Kotlin UI fina + NDK core)
  app/src/main/cpp/           Core NDK: câmera (ACamera) -> AMediaCodec (surface) -> rede
protocol/                     Protocolo binário v1 (C++17 portátil) + testes
  include/owc/protocol.h      CONTRATO — fonte da verdade
windows/
  common/include/owc/shm.h    CONTRATO — barramento de frames em memória compartilhada (NV12)
  core/                       owc_core: conexão, depacketizer, decoder MF HW, ShmWriter, adb, discovery
  app/                        OWC.exe: UI Dear ImGui + D3D11 (preview, controles, stats)
  virtualcam/                 owc_virtualcam.dll: filtro DirectShow (webcam virtual Win10/11)
  obs-plugin/                 owc-obs-source.dll: fonte assíncrona no OBS (lê shm)
  tools/                      owc_testsrc.exe (produz padrão de teste no shm), owc_shmview
scripts/                      build/instalação
```

Fluxo de vídeo:

```
[Sensor] -> ACameraCaptureSession -> ANativeWindow(input surface do encoder)  (zero-copy)
  -> AMediaCodec H.264/HEVC HW (baixa latência, sem B-frames)
  -> fila de envio (máx ~2 frames; overflow => descarta até próximo IDR + pede IDR)
  -> TCP (controle+vídeo) | UDP (fragmentos 1400B) | USB (= TCP via adb forward)
  -> PC: FrameAssembler -> Media Foundation decoder (DXVA, CODECAPI_AVLowLatencyMode)
  -> NV12 -> ShmWriter  -> owc_virtualcam.dll (Zoom/Teams/Chrome/OBS "Video Capture Device")
                        -> owc-obs-source.dll (OBS, unbuffered)
                        -> preview no OWC.exe
```

## Decisões

| Tema | Decisão | Motivo |
|---|---|---|
| minSdk | 26 (Android 8) | `AMediaCodec_createInputSurface` (zero-copy) exige 26 |
| ABI | arm64-v8a, armeabi-v7a | celulares antigos 32-bit |
| Codec | H.264 padrão, HEVC opcional | H.264 HW universal; decoder MF sempre presente |
| Encoder | Baseline/sem B-frames, CBR, `priority=0`, `latency=1` (API30+), extensões qcom low-latency quando houver, IDR sob demanda (`request-sync`), bitrate ao vivo (`video-bitrate`) | latência mínima |
| Rede | TCP_NODELAY, SO_SNDBUF pequeno, DSCP EF no UDP, Wi-Fi lock LOW_LATENCY (API29+) | evita bufferbloat |
| UDP | Phone escuta 47801; PC envia Punch, phone responde no endereço de origem | não precisa regra de firewall no Windows |
| USB | `adb forward tcp:X tcp:47800` (adb do SDK/scrcpy/PATH) | sem driver extra |
| Tela apagada | Foreground service `foregroundServiceType="camera"` + partial wakelock + modo "tela preta" (brilho mínimo, OLED ≈ desligado) + pedir isenção de otimização de bateria | Camera continua com tela desligada |
| Webcam virtual | Filtro DirectShow próprio (COM, regsvr32) | Win10 (MFCreateVirtualCamera é só Win11) |
| OBS | Plugin nativo (fonte async, `obs_source_set_async_unbuffered`) lendo shm | menor latência que capturar a webcam virtual |
| UI Windows | Dear ImGui + D3D11 + Win32, C++ | um binário, zero runtime extra |
| Toolchain Win | VS2019 Build Tools MSVC 14.29, SDK 10.0.19041, CRT estático | é o que está instalado |

## Portas

| Porta | Proto | Uso |
|---|---|---|
| 47800 | TCP | controle (+ vídeo no modo TCP/USB) |
| 47801 | UDP | vídeo UDP (punch) |
| 47802 | UDP | descoberta por broadcast |

## Contrato JNI (Android)

Classe `com.owc.webcam.NativeBridge` (Kotlin `object`, todos os métodos `@JvmStatic`),
biblioteca `libowc_native.so`:

```kotlin
object NativeBridge {
    init { System.loadLibrary("owc_native") }
    @JvmStatic external fun nativeStart(deviceName: String, model: String, apiLevel: Int,
                                        controlPort: Int, udpPort: Int, discoveryPort: Int): Boolean
    @JvmStatic external fun nativeStop()
    @JvmStatic external fun nativeSetPreviewSurface(surface: Surface?)   // null = sem preview local
    @JvmStatic external fun nativeGetStatus(): String                      // JSON (abaixo)
    @JvmStatic external fun nativeUpdateDeviceStatus(batteryPct: Int, batteryTempC: Float,
                                                     charging: Boolean, thermalStatus: Int)
    @JvmStatic external fun nativeSetDeviceName(name: String)

    // chamado pelo nativo (qualquer thread; o nativo faz AttachCurrentThread)
    @JvmStatic fun onNativeEvent(type: Int, intArg: Int, strArg: String?)
}
```

Eventos (`type`): `1` CLIENT_CONNECTED (str = peer), `2` CLIENT_DISCONNECTED,
`3` SCREEN_MODE (int = 0 normal / 1 preta), `4` STREAMING_STARTED, `5` STREAMING_STOPPED,
`6` ERROR (str = mensagem), `7` DEVICE_NAME (str = novo nome definido pelo PC; o app persiste).

`nativeGetStatus()`:
```json
{"state":"idle|listening|connected|streaming","peer":"192.168.0.10:51234",
 "transport":"tcp|udp","cameraId":"0","width":1920,"height":1080,"fps":30.0,
 "codec":"h264","bitrateKbps":8000,"dropped":0,"encodeLatencyMs":12.5}
```

JNI: nomes `Java_com_owc_webcam_NativeBridge_native*` com `jclass` (estático).
`onNativeEvent` assinatura `(IILjava/lang/String;)V`. O nativo guarda `JavaVM*`
em `JNI_OnLoad` e um `GlobalRef` da classe.

## Política de latência

**Tudo é configurável pelo Windows.** O padrão é latência mínima, mas o usuário pode
trocar latência por qualidade (fila maior, B-frames, perfil High, QP, jitter buffer
no PC, FEC, etc.). A UI oferece presets — *Ultra baixa latência*, *Equilibrado*,
*Qualidade máxima* — e modo manual expondo cada parâmetro (`ParamId` no phone +
opções locais do PC: jitter buffer ms, decoder HW/SW, pacing, transformações).
Os itens abaixo descrevem o preset padrão (latência):

1. Câmera → encoder sem cópia (surface).
2. Encoder: thread dedicada `dequeueOutputBuffer` timeout curto, prioridade alta.
3. Fila de envio ≤ 2 frames. Se encher: descarta até o próximo keyframe, chama
   `request-sync`, conta `dropped_frames`.
4. UDP: perda → PC descarta deltas até IDR e envia `RequestKeyframe` (rate-limit 250 ms).
5. PC: decodifica imediatamente, publica o frame mais novo; nada de jitter buffer.
6. Medição: `capture_ts_us` (relógio do phone) + offset estimado via Ping/Pong →
   latência captura→exibição mostrada na UI.

## Build

Windows (cada componente pode ser compilado isolado):
```
cmake -S windows -B build/win -G "Visual Studio 16 2019" -A x64
cmake --build build/win --config Release
ctest --test-dir build/win -C Release
```
CMake: `C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`

Android:
```
cd android && .\gradlew.bat assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```
