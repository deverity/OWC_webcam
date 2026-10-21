# OWC — Latência avançada no lado do aparelho (Galaxy Note 9 SM-N9600, SDM845, Android 10)

Escopo: técnicas **no dispositivo** para reduzir a latência captura→PC e melhorar a
robustez, fundamentadas no nosso código (`android/app/src/main/cpp/**`,
`hs/HighSpeedCamera.java`, `WebcamService.kt`), em inspeção read-only do aparelho
(`adb shell dumpsys/getprop/logcat`, §3) e nas medições do próprio core
(`Worker::logLatencyBreakdown`, `session.cpp:765`). Complementa
`docs/LOW_LATENCY_RESEARCH.md` (scrcpy/Moonlight/Sunshine) — nada daquilo é repetido.

Convenções: **[verificado]** = lido no aparelho/código; **[não verificado]** = conhecimento
de AOSP/CodeAurora/Qualcomm que precisa de teste; ganhos são estimativas sobre a linha de
base medida (normal 1080p30 ≈ 112–130 ms captura→PC; high-speed 720p60 ≈ 47–55 ms;
HS 1080p60 ≈ 56–61 ms; 4K30 ≈ 160 ms).

Firmware inspecionado: `QP1A.190711.020.N9600ZHU9FVH2` (patch 2022-06-01), kernel
`4.9.186-22986754`, HAL `vendor.samsung.hardware.camera.provider@3.0-service` (CamX/CHI),
encoder `OMX.qcom.video.encoder.avc/hevc` em `media.codec` (OMX, não Codec2).

---

## 0. Onde a latência está (modelo físico + o que medimos)

`capture_ts_us` = `ACAMERA_SENSOR_TIMESTAMP` = **início da exposição da 1ª linha**. Entre
ele e "ISP done" (`onCaptureCompleted`) estão, em série:

1. **Exposição** `T_exp` (AE em [30,30] vai até 33 ms em ambiente interno; com manual
   4 ms vocês mediram −25 ms — coerente). Perceptualmente só `T_exp/2` conta (o
   "instante" da imagem é o meio da exposição), então a métrica atual **superestima**
   a latência percebida em até ~16 ms com AE automático.
2. **Readout rolling-shutter** `T_ro`: este sensor faz 4K@60 e 1080p@240 (listas
   Samsung, §3.1), logo `T_ro` ≤ 16,7 ms no modo full-FOV e ≈ 4 ms nos modos HFR.
   `ACAMERA_SENSOR_ROLLING_SHUTTER_SKEW` **não** está em `availableResultKeys`
   [verificado] — não dá para ler; usar os limites acima.
3. **Pipeline CamX** (IFE→IPE→retorno): `pipelineMaxDepth=8`, mas o que importa é que
   os estágios são sincronizados ao **período de quadro**. Evidência: HS 240 fps dá
   "ISP done" 21–29 ms = lote de 4 quadros (16,7 ms, §3.2) + ~5–12 ms; normal 30 fps
   dá 80–104 ms ≈ 2,5 períodos + exposição. **A alavanca dominante é o fps do sensor**,
   não os knobs de processamento.
4. **Hand-off para o encoder** (BufferQueue, zero-copy, UBWC): sub-ms.
5. **Encode (Venus)**: +10–20 ms a 1080p30, +32–35 ms a 4K30 [verificado em logcat].
   O firmware escala o clock para "caber" no fps declarado (`operating-rate`/`frame-rate`
   = 30) → o encode tende a durar ~1 período de quadro independentemente da resolução.
6. **Envio**: +1–7 ms (inclui o nosso pacer UDP, §2.5).

Piso físico a 30 fps com exposição 4 ms ≈ 4 + 16 + ~5 (IFE) + ~10 (IPE) ≈ 35–40 ms;
a 60 fps ≈ 30 ms; o resto (≈ 1 período de quadro) é pipelining do HAL que só cai
subindo o fps.

---

## 1. (a) Tabela ranqueada de propostas

| # | Proposta | Ganho esperado (captura→PC) e raciocínio | Confiança | Esforço | Risco | Toca em |
|---|---|---|---|---|---|---|
| 1 | **Sessão normal a 60 fps** (AE range [60,60] não anunciado / `SENSOR_FRAME_DURATION` manual / vendor tags Samsung) | **−35 a −45 ms** no normal 1080p (ISP 80–104 → ~45–60: pipeline escala com o período; AE a [60,60] ainda limita a exposição a 16,7 ms = −8 ms extra). Sem lotes, controles completos, preview local possível | baixa-média (0,35): `availableMinFrameDurations` = 16,67 ms para todos os tamanhos e a lista Samsung anuncia 1080p60/4K60 [verificado], mas o HAL pode recusar | médio | ERROR_CAMERA_DEVICE / request failed → fallback automático | `camera_info.cpp:197-199`, `params.cpp:148-157`, `camera_controls.cpp:89-100`, novo helper Java ou `ACameraManager_getTagFromName` |
| 2 | **HS: tirar o "VSync hold" do preview fantasma** (`USAGE_GPU_SAMPLED_IMAGE` → `USAGE_VIDEO_ENCODE`/outro) | **−10 a −15 ms médios e −33 ms no p90** em HS 720p60/1080p60: o cameraserver Samsung segura o buffer do stream HW_TEXTURE até 33 ms na thread de callback do HAL (§3.3); e é justamente o 1º quadro de cada lote (o que `max-fps-to-encoder` mantém) que tem o alvo de preview | média (0,5) | trivial (1 constante) | HAL exigir "preview" com textura e voltar a travar | `hs/HighSpeedCamera.java:181-182` |
| 3 | **AE com prioridade de exposição** (`org.codeaurora.qcamera3.iso_exp_priority.*`): teto de exposição 4–8 ms com ganho automático | **−10 a −25 ms** em luz interna (igual ao ganho medido do manual 4 ms, sem perder AE) + −12 ms "percebidos" | média (0,5): tags e `exposure_time_range` anunciados [verificado]; semântica [não verificado] | baixo-médio | tag ignorada (sem efeito); ruído alto | `camera_controls.cpp::applyControls`, `params.cpp` (novo ParamId), tag via `ACameraManager_getTagFromName` |
| 4 | **Clock do Venus**: `operating-rate` = 2–4× fps (1080p: 120–240; 4K: 60), manter `priority=0` | **−5 a −10 ms** 1080p30 (hop 10–20 → 5–10), **−15 a −20 ms** 4K30 (35 → 15–20): o driver `msm_vidc` vota clock pela operating-rate; hoje = fps | média (0,5) [não verificado no aparelho] | trivial | configure falhar (escada já trata); +consumo/térmico | `encoder_options.cpp:195-196` |
| 5 | **Pacer UDP mais rápido + DSCP no TCP** (piso 20 Mbps/4× → ~100 Mbps em 5 GHz ou 50 % do link; `IP_TOS`+`SO_PRIORITY` no socket TCP) | **−3 a −6 ms por quadro** (33 KB a 32 Mbps = 8,3 ms de serialização que nós mesmos impomos) e **−25 a −30 ms em IDR** (150 KB) no Wi-Fi/UDP; AC_VO para o vídeo TCP no Wi-Fi | alta (aritmética) | trivial | rajadas em 2.4 GHz fraco → FEC/ABR; parametrizar | `sender.cpp:20-22,174-177,267-288`, `socket_util.cpp:71-83` |
| 6 | **Intra-refresh em vez de IDR periódico (+ CBR no AVC)** no preset Wi-Fi | remove o pico periódico de **+20 a +40 ms** (p99) do IDR de 150 KB a cada 2 s; CBR limita o tamanho de quadro (jitter) | média-alta (OMX.qcom suporta `intra-refresh-period`; CBR do AVC [não verificado] — MediaCodecList diz VBR-only, XML não declara) | baixo | qualidade de recuperação após perda | `params.cpp:102-106` (defaults), `encoder_options.cpp:168-173,205-209` |
| 7 | **Wi-Fi ocioso**: `BlackScreenActivity` (foreground real + tela "ligada" a 0 nits) por padrão em Wi-Fi + keepalive phone→PC 5–10 Hz com DSCP EF; orientar 5 GHz | elimina os RTT de **0,5–1,7 s** em idle (DTIM skipping com tela apagada; `FULL_LOW_LATENCY` só vale com tela ligada + app em foreground — overlay não conta) → start-stream e tap-to-focus instantâneos | média (0,5) | baixo | consumo de tela mínimo | `WebcamService.kt:179-206,343-361`, `net_server.cpp` (keepalive) |
| 8 | **Caminho de saída do encoder sem cópias/polling**: callback assíncrono, 1 cópia em vez de 3, `TCP_NOTSENT_LOWAT` em vez de `poll` de 2 ms, afinidade opcional nos big cores | **−0,5 a −2,5 ms** (sobretudo jitter) | alta | baixo | nenhum | `video_encoder.cpp:304-357`, `sender.cpp:250-261,397-405` |
| 9 | Knobs de processamento por request (hotPixel/shading/tonemap/aberration FAST ou OFF, faceDetect OFF, sceneMode DISABLED, NR/edge OFF, TNR vendor off, `NodeBypass` Samsung) | 0 a −5 ms (desconhecido; CamX processa num passe de IPE) | baixa | baixo (tabela) | nenhum | `camera_controls.cpp:165-173` |
| 10 | Instrumentação: `ACAMERA_REQUEST_PIPELINE_DEPTH` e `SENSOR_EXPOSURE_TIME` por resultado; "latência efetiva" = medida − `T_exp/2`; calibração buffer vs resultado com `AImageReader` | não reduz latência; torna os experimentos 1–4 decidíveis | alta | baixo | nenhum | `camera_stream.cpp:42-55`, `session.cpp:765-774` |
| 11 | HEVC + CBR como preset Wi-Fi quando o PC tiver decoder HW HEVC | −2 a −5 ms de transmissão (−40 % bytes), IDR menores | média | baixo | PC sem "HEVC Video Extensions" | `params.cpp:101` |
| 12 | USB **tethering RNDIS** como transporte alternativo (phone 192.168.42.129) | −1 a −3 ms vs `adb forward` (sem relay adbd/USB stop-and-wait, §3.6) + UDP/FEC em USB, sem depender do adb server | média | médio (UX: usuário liga tethering) | Windows "sem internet"; driver RNDIS | `windows/core` (descoberta já cobre `rndis0`), docs |
| 13 | LTR (`vendor.qti-ext-enc-ltr.*`) para recuperação de perda sem IDR | robustez UDP: recuperação com P-frame normal em vez de IDR 5–10× maior | baixa (chaves [não verificado] no SDM845) | médio-alto | — | `encoder_options.cpp`, protocolo (novo Command) |
| 14 | Fila HS dimensionada pelo lote real (batch 4, não 8) | evita até +100 ms de backlog permitido em congestão | alta | trivial | — | `session.cpp:703-711` |

Ordem de execução sugerida: 10 → 2 → 4 → 5 → 3 → 1 → 6/7 → 8/14 → 9/11/12.

---

## 2. (b) Top-8: esboço de implementação + desenho do experimento

Ferramentas comuns a todos os experimentos:

- Medição no phone: `adb logcat -s OWC | grep "latency breakdown"` (EWMA a cada 5 s:
  `sensor->ISP done | ->encoder out (+x) | ->sent (+y)`), `OWC.HS` ("sensor frame
  spacing"), e para o PC `build\verify-core\out\Release\owc_core_probe.exe --connect
  127.0.0.1:<porta adb> --seconds 60 --set id=valor` (ids em `protocol.h::ParamId`;
  prefixos `B/L/F/E`) ou `owc_probe.py host porta segundos out.h264 id=valor`.
- Sempre A/B na mesma cena/iluminação, ≥ 60 s por braço, anotar `thermal_status`.
- Confirmações read-only do lado do sistema: `adb logcat -d | grep -c "VSync\] returnBufferCheckedLocked"`,
  `adb logcat -d | grep -E "OMX-VENC|OMXNodeInstance|Camera3-Device"`.

### 2.1 Sessão normal a 60 fps (P1)

Fatos: `android.control.aeAvailableTargetFpsRanges` = {[15,15],[6,24],[24,24],[7,30],
[15,30],[30,30]}; `availableMinFrameDurations` = 16 666 666 ns para 1080p/720p/4K;
`samsung.android.scaler.availableVideoConfigurations` lista `1920x1080 [60,60]`,
`3840x2160 [60,60]`, `3840x1644 [60,60]`; `availableRequestKeys` só contém tags
padrão (nenhuma vendor) [verificado, §3.1]. O HAL é 3.3 (sem session parameters).
Vendor tags Samsung potencialmente relevantes (nomes exatos do `dumpsys`, §3.1):
`samsung.android.control.recordingMotionSpeedMode` (int32), `samsung.android.control.recordingHint` (byte),
`samsung.android.control.samsungCamera` (byte), `samsung.android.uniplugin.fps` (int32),
`samsung.android.control.repeatingRequestHint` (int32), `samsung.android.control.captureHint` (int32).

Implementação:
1. `camera_info.cpp::describe` (L197-199): parar de truncar `max_fps` por `maxAeFps`;
   guardar `minFrameDurationNs` por tamanho e expor `max_fps = 1e9/minFrameDuration`
   quando `manualSensor` (nova flag `CameraDesc::fpsBeyondAe`).
2. `params.cpp::fpsRange` (L155-156): permitir até esse teto quando `controls.aeMode==0`
   **ou** quando um novo `ParamId::SensorFpsMode` (Enum 0=AE 1=manual-duration
   2=vendor-samsung) estiver ≠ 0.
3. `camera_controls.cpp::applyControls` (L89-100): modo 1 → `ACAMERA_SENSOR_FRAME_DURATION
   = 1e9/fps` com AE OFF (já existe) **e** AE range = o maior anunciado; modo 2 →
   `ACAMERA_CONTROL_AE_TARGET_FPS_RANGE = [fps,fps]` mesmo não anunciado + tags vendor.
4. Tags vendor pelo NDK: `ACameraManager_getTagFromName(mgr, id, name, &tag)` (API 29,
   `NdkCameraManager.h:412` [verificado no NDK 27]; resolver por `dlsym` para manter
   minSdk 26) e `ACaptureRequest_setEntry_i32/u8(req, tag, …)`. [não verificado]: o NDK
   aceita tags vendor em `ACameraMetadata::isCaptureRequestTag` (há um atalho
   `isVendorTag → true` em AOSP P+). Fallback certo: helper Java como `HighSpeedCamera`
   usando `new CaptureRequest.Key<>(name, Integer.class)` (construtor público desde API 29).
5. Para varreduras sem rebuild: `CommandId::SetVendorTag = 7` com `text = "name=i32:5"`
   aplicado ao repeating request (append-only no protocolo).

Experimento (matriz, 1080p, cena fixa, 60 s cada):

| Braço | Request | Sucesso = |
|---|---|---|
| E1 | AE ON, range [60,60] | `takeFrameCount` ≈ 60/s, Δ`SENSOR_TIMESTAMP` ≈ 16,7 ms, sem `onCaptureFailed`; breakdown ISP ≤ 60 ms |
| E2 | AE OFF, `SENSOR_FRAME_DURATION`=16 666 666, exposição 8 ms, range [30,30] | idem |
| E3 | E1 + `recordingMotionSpeedMode` ∈ {0..4} | idem |
| E4 | E1 + `recordingHint=1`, `samsungCamera=1`, `uniplugin.fps=60` | idem |
| E5 | E2 + 4K (`3840x2160`) | 4K ISP < 80 ms |

Se nenhum braço passar, o teto de 60 fps fica restrito ao modo HS (já implementado) e
a proposta é descartada sem custo residual.

### 2.2 HS: eliminar o VSync hold do preview fantasma (P2)

Fatos [verificado, §3.3]: na sessão HS o stream 0 é o `ImageReader` (consumer usage
256 = `GRALLOC_USAGE_HW_TEXTURE`, "Forcing asynchronous mode"), o stream 1 é o encoder
(65536 = `HW_VIDEO_ENCODER`). O cameraserver Samsung loga, nas **mesmas threads que
entregam os resultados do HAL** (tids 1373/1376/23127), `[VSync] returnBufferCheckedLocked:
Stream 0: Return buffer buffer wait time exceeded. (33)ms base(8)ms` ~12×/s (≈40 % dos
buffers de preview), e `Camera3-Stream: [VSync] scheduleVsync … diff(33)ms starvation
count (N)`. Um stream de textura sem display real espera um VSync que não chega e
estoura em ~33 ms, bloqueando a thread de callback. Como `max-fps-to-encoder` (dropper
do `GraphicBufferSource`, tolerância 2 ms) mantém o quadro mais próximo do múltiplo de
16,7 ms — o 1º de cada lote, exatamente o que carrega o alvo de preview —, a hipótese
é uma distribuição bimodal (0 / +33 ms em ~40 % dos quadros) que a EWMA mostra como
+12–15 ms. Na sessão normal sem preview (stream único HW_VIDEO_ENCODER) **não há**
linhas VSync [verificado], mas com o preview local do app (SurfaceView = textura +
composer) o hold volta (base 8 ms com VSync real → +8–16 ms).

Implementação: `HighSpeedCamera.java:181` → `ImageReader.newInstance(w, h,
ImageFormat.PRIVATE, 3, HardwareBuffer.USAGE_VIDEO_ENCODE)`; se o HAL recusar
(configuração falha ou "buffer error"), tentar `USAGE_COMPOSER_OVERLAY` e
`USAGE_GPU_COLOR_OUTPUT`. Para a sessão normal: `MainActivity` já deixa o preview
desligado por padrão; documentar que ligar o preview custa latência e, se quiser preview
sem custo, usar um `AImageReader` de 30 fps **sem** usage de textura e desenhar via GL
a partir dele (não via SurfaceView direto na câmera).

Experimento: 4 braços (usage atual, VIDEO_ENCODE, COMPOSER_OVERLAY, GPU_COLOR_OUTPUT)
em HS 720p60 e 1080p60; métricas: `->encoder out`, contagem de linhas `VSync]` por
minuto (esperado 0 nos braços novos), `camera_fps` estável, ausência de "A frame is
dropped … buffer error". Depois, normal 1080p30 com preview local ON vs OFF.

### 2.3 AE com prioridade de exposição (P3)

Fatos [verificado, §3.1]: a seção `org.codeaurora.qcamera3.iso_exp_priority` existe
(`use_iso_exp_priority` int64, `use_iso_value` int32, `select_priority` int32,
`iso_available_modes` [0..7], `exposure_time_range` [40 000, 100 000 000] ns) e os
ranges são publicados nas características da câmera 0 — forte indício de que o HAL
implementa. Semântica (SnapdragonCamera `CaptureModule`, [não verificado]):
`select_priority = 1` → prioridade de exposição e `use_iso_exp_priority` = tempo em ns;
`= 0` → prioridade de ISO com índice de modo. Alternativa 100 % padrão: só o fps range
já limita a exposição (`[60,60]` → ≤ 16,7 ms) — motivo adicional para o P1.

Implementação: novo `ParamId::ExposureCap = 36` (Int64 ns, 0 = off) em `params.cpp`;
em `applyControls`, se `cap > 0 && aeMode == 1`: setar as duas tags vendor; manter
`ACAMERA_CONTROL_AE_MODE_ON`. Ler `ACAMERA_SENSOR_EXPOSURE_TIME`/`SENSITIVITY` em
`onCaptureCompleted` (P10) para confirmar no log.

Experimento: sala com ~100 lux; braços: AE auto; AE auto + cap 8 ms; AE auto + cap 4 ms;
manual 4 ms (`--set 22=E0 --set 26=L4000000`). Métricas: breakdown (ISP), exposição e
ISO reportados, bitrate (ruído), fps real. Sucesso = exposição ≤ cap com ISO subindo e
ISP ≈ o braço manual.

### 2.4 Clock do encoder (P4)

Fatos [verificado, logcat §3.4]: `OMX-VENC: ERROR: unsupported index 1870659589`
(= 0x6F800005 `OMX_IndexConfigLatency`) + `OMXNodeInstance: getConfig(… ConfigLatency)`
→ a chave `latency=1` **é no-op** neste encoder. `vendor.qti-ext-enc-low-latency.enable=1`
é aceita (ecoada no formato de entrada). `operating-rate=30/60/120` é aplicada
(`ACodec` dump). XML do vendor: `frame-rate` 1–480, performance points
`1920x1088@240`, `1280x720@480`, `3840x2160@60`.

Implementação: `encoder_options.cpp:195-196` → `v = std::min(perfPointFps(w,h), s.fps * k)`
com `k` default 4 (1080p → 120, 4K → 60) e novo `ParamId::EncoderOperatingRate` (0 = auto)
para sweeps. Manter `priority=0`. Não tocar `frame-rate` (afeta o rate-control).

Experimento: normal 1080p30 e 4K30, `operating-rate` ∈ {30, 60, 120, 240}; também
`vendor.qti-ext-enc-low-latency.enable` ∈ {0,1} em cada um (hoje nunca se confirmou que
ela vale algo). Métricas: `->encoder out (+x)`, `thermal_status`, bitrate real
(`rateControlCheck`). Sucesso = +x cai para ~1/3 do período de quadro.

### 2.5 Pacer UDP e DSCP no TCP (P5)

Fatos: `sender.cpp:22` `kMinPacingBps = 20 Mbps`, `setPacingBitrate` usa `4 × bitrate`;
token bucket de 64 KB (`kBurstBytes`). A 8 Mbps o pacer libera 32 Mbps: quadro P de
33 KB leva 8,3 ms para sair; IDR de 150 KB, 37 ms. `tuneTcpClient` não marca DSCP
(`socket_util.cpp:71-83`); qdisc `pfifo_fast` em `wlan0` [verificado] mapeia pelo
`skb->priority`, e o driver qcacld mapeia TOS→AC (EF → AC_VO).

Implementação: (1) pacing = `max(floor, 8 × bitrate)` com `floor` = 100 Mbps se o link
reportado (`WifiInfo.getLinkSpeed()`, passado via `nativeUpdateDeviceStatus` ampliado ou
novo JNI) ≥ 150 Mbps, senão 40 Mbps; expor `ParamId::PacingMultiplier`. (2) `kBurstBytes`
= 1 IDR médio (≈ 128 KB). (3) `tuneTcpClient`: `setDscpEf(fd)` + `setsockopt(SO_PRIORITY, 6)`;
no UDP também `SO_PRIORITY`.

Experimento: Wi-Fi UDP 1080p30 8 Mbps, pacing 32 vs 100 Mbps: `->sent (+y)`,
`ReceiverReport.fragments_lost`, `jitter_ms`, `frames_recovered` (FEC). Em TCP Wi-Fi:
RTT/jitter com e sem DSCP (`dumpsys wifi` não mostra AC; medir).

### 2.6 Intra-refresh + CBR no preset Wi-Fi (P6)

Implementação: preset "Wi-Fi baixa latência" = `KeyframeIntervalMs=0`,
`IntraRefreshFrames = fps` (1 s), `RequestKeyframe` continua forçando IDR em perda
irrecuperável; em `params.cpp:106` tentar CBR no AVC no stage 0 mesmo quando
`bitrateModes` diz que não, verificando o eco de `bitrate-mode` em
`AMediaCodec_getOutputFormat` (o `tryConfigure` do probe já faz isso para vendor keys).
HEVC declara `VBR,CBR,CQ` no XML [verificado].

Experimento: 120 s Wi-Fi UDP; comparar p50/p99 da latência no PC e desvio-padrão do
tamanho de quadro com IDR 2 s vs intra-refresh; checar `BUFFER_FLAG_KEY_FRAME` (nenhum
periódico) e recuperação após perda induzida (`--jitter`/perda no probe).

### 2.7 Wi-Fi ocioso: tela, locks e keepalive (P7)

Fatos [verificado, §3.5]: `dumpsys wifi` mostra os dois locks do OWC "held",
`PowerSave:true`, phone em **2412 MHz (2,4 GHz)** com o AP também em 5745 MHz;
`source.android.com` documenta que `WIFI_MODE_FULL_LOW_LATENCY` só ativa com Wi-Fi
conectado, app em **foreground** e **tela ligada**. O overlay `TYPE_APPLICATION_OVERLAY`
(`WebcamService.showOverlay`) não torna o app foreground; `BlackScreenActivity`
(`FLAG_KEEP_SCREEN_ON` + `BRIGHTNESS_OVERRIDE_OFF`) torna, e mantém a tela "ligada" —
o que também desarma o DTIM skipping do firmware (listen interval multiplicado com tela
apagada, a causa provável dos 0,5–1,7 s).

Implementação: em `WebcamService.openBlackScreen` preferir a Activity quando o
transporte for Wi-Fi (o overlay só como fallback); keepalive UDP de 8 bytes phone→PC a
5–10 Hz com DSCP EF enquanto `connected && !streaming` (UAPSD trigger em AC_VO, se o AP
tiver WMM-PS); mostrar banda/frequência no `Stats` (novo campo opcional) e avisar
"use 5 GHz".

Experimento: RTT (Ping/Pong) em idle por 5 min em 4 estados: tela apagada+overlay;
BlackScreenActivity; tela ligada; cada um com/sem keepalive. Sucesso = p99 RTT < 50 ms.

### 2.8 Caminho de saída do encoder (P8)

Implementação: `video_encoder.cpp` — `AMediaCodec_setAsyncNotifyCallback` (API 28,
`dlsym` para minSdk 26) com `onAsyncOutputAvailable` criando o `EncodedFrame`
diretamente do buffer (1 cópia; remover `tmp` em L323 e o `assign` em L286/290);
`sender.cpp::sendVideoTcp` → `iovec[3] = {TcpHeader, VideoFrameHeader(17 B), au}` sem
`tcpBuf_`; `sender.cpp:397-405` → `TCP_NOTSENT_LOWAT = 1,5 × quadro médio` e
`poll(POLLOUT)` sem o `wait_for(2 ms)`; opcional `sched_setaffinity` (cpuset
`/foreground` = 0-7 [verificado], big cores 4-7) para `owc-enc-out` e `owc-send`.
Ganho pequeno; útil para baixar jitter e simplificar.

---

## 3. (c) Inspeção read-only do aparelho (comandos e achados)

### 3.1 Câmera (`adb shell dumpsys media.camera`, salvo em scratchpad `dev/media_camera.txt`)

Câmera 0 (traseira, HW level 3, `timestampSource=REALTIME`, `pipelineMaxDepth=8`,
`sync.maxLatency=PER_FRAME_CONTROL`, `maxNumOutputStreams=[1 3 2]`):

- `aeAvailableTargetFpsRanges`: [15,15] [6,24] [24,24] [7,30] [15,30] [30,30].
- `availableMinFrameDurations`: 16 666 666 ns em **todos** os tamanhos PRIVATE/YUV
  (inclusive 3840x2160), 33 333 333 apenas em 4032x3024.
- `availableHighSpeedVideoConfigurations` = `[1920 1080 240 240 **4**]`,
  `[1280 720 240 240 **4**]` → `batch_size_max = 4` (cameraserver: "Matching high speed
  configuration found. Limit batch size to 4" [verificado]) — os lotes são de **4 quadros
  / 16,7 ms**, não de 8 / 33 ms.
- Samsung privadas: `samsung.android.scaler.availableHighSpeedVideoConfigurations` =
  1080p [120,120] e [240,240], 720p [120,120] e [240,240];
  `availableVideoConfigurations` inclui 1920x1080 [60,60], 3840x2160 [60,60],
  3840x1644 [60,60]; `effectAeAvailableTargetFpsRanges` = [10,30] [30,30].
- Modos: NR [0 1 2 3 4], edge [1 2 0 3], hotPixel [0 1 2], shading [0 1 2], tonemap
  [0 1 2], aberration [0 1 2], faceDetect [0 1], lensShadingMap [0 1], OIS [0 1],
  videoStab [0 1]; capabilities incluem MANUAL_SENSOR, MANUAL_POST_PROCESSING,
  CONSTRAINED_HIGH_SPEED_VIDEO, READ_SENSOR_SETTINGS.
- `availableRequestKeys` (57, todas padrão; decodificadas): control.* (0x10000-0x10011
  menos awbRegions, + postRawSensitivityBoost), colorCorrection.mode/transform/gains/
  aberrationMode, edge.mode, flash.mode, hotPixel.mode, noiseReduction.mode,
  scaler.cropRegion, sensor.exposureTime/frameDuration/sensitivity, shading.mode,
  statistics.faceDetectMode/hotPixelMapMode/lensShadingMapMode, tonemap.*, lens.*.
  **Não** há `android.sensor.rollingShutterSkew` nem `android.request.pipelineDepth` nas
  listas publicadas (o segundo é obrigatório e deve vir mesmo assim — testar).
- Vendor tags (321): seções `org.codeaurora.qcamera3.*` (`iso_exp_priority`,
  `instant_aec`, `temporal_denoise.enable/process_type`, `exposure_metering`,
  `sharpness`, `saturation`, `histogram`, `av_timer`), `org.quic.camera.*`
  (`eis3enable.EISV3Enable`, `qtimer.timestamp`, `streamDimension`, `ifecropinfo`),
  `com.qti.chi.*` (`multicamerainfo.LowPowerMode`, `statsSkip.skipFrame`),
  `com.samsung.node.uniplugin_{preview,record,capture}.NodeBypass` (byte),
  `samsung.android.control.*` (68 tags; ver lista completa no dump, linhas 6324-6391).

### 3.2 Codec (`/vendor/etc/media_codecs_vendor.xml`, `media_codecs_performance.xml`)

- `OMX.qcom.video.encoder.avc`: size 96x96..4096x2304, `frame-rate` 1–480,
  `blocks-per-second` ≤ 2 073 600, bitrate ≤ 120 Mbps, performance points 4096x2304@30,
  3840x2160@60, 1920x1088@240, 1280x720@480; **sem** `Feature bitrate-modes` (por isso
  o MediaCodecList diz VBR-only). `OMX.qcom.video.encoder.hevc`: idem + `bitrate-modes
  VBR,CBR,CQ`. `max-video-encoder-input-buffers = 11` (bate com "Consumer wants 11
  buffers" no cameraserver).
- `measured-frame-rate-1920x1080` = 20–44 (avc), 17–37 (hevc) — números de CTS, não
  limite de HW.
- `/vendor/lib64/libOmxVenc.so`: `strings`/`grep -a` → **Permission denied** (SELinux
  para `shell`); a lista de extensões aceitas vem do nosso probe
  (`encoder_options.cpp::probeOne`, linha "probe: … formats:"), que já ecoa todas as
  `vendor.*` que o componente reporta.

### 3.3 Cameraserver Samsung (`adb logcat -d`)

- Normal 4K30: `Creating new stream 0: 3840 x 2160, format 34, dataspace 260 …`,
  `consumer usage flag: 65536` (HW_VIDEO_ENCODER), `configureStreams: stream[0] …
  max_buffers = 8`, `Consumer wants 11 buffers, HAL wants 8`, `operatingMode=0`,
  template 3 (RECORD). Nenhuma linha `[VSync]`.
- HS 720p: stream 0 = ImageReader (usage 256 HW_TEXTURE, "Forcing asynchronous mode
  for stream"), stream 1 = encoder (65536, dataspace 0x104), `max_buffers = 32` nos
  dois, `operatingMode=1`, "Limit max batch size to 4".
- `[VSync] returnBufferCheckedLocked: Stream 0: Return buffer buffer wait time exceeded.
  (33)ms base(8)ms` — 751 ocorrências/min em HS (≈12/s), tids 1373/1376/23127 = as
  mesmas das linhas "First frame for stream 0/1"/"frame is dropped"; e `Camera3-Stream:
  [VSync] scheduleVsync frame returend over vsync target time diff(33)ms starvation
  count (N)` + `Max/Min VsyncPeriod has updated 16646000`.
- `Camera3-OutputStream: … A frame is dropped for stream 1 due to buffer error` nos
  primeiros quadros da sessão HS (transitório).

### 3.4 OMX/ACodec (`adb logcat -d | grep -E "OMX-VENC|OMXNodeInstance|ACodec"`)

- `OMX-VENC: ERROR: unsupported index 1870659589` (0x6F800005) e
  `OMXNodeInstance: getConfig(…, ConfigLatency(0x6f800005)) (0x8000101a)` → `latency`
  não suportada (OMX_ErrorUnsupportedIndex).
- `venc_dev: Unsupported eColorFormat 0x7f000789` (negociação do surface; benigno),
  `component does not support metadata mode; using fallback` (porta de saída),
  `Failed to update the profile_level` (o `level` que passamos pode não aplicar).
- Formato aplicado (dump do ACodec): `priority=0 latency=1 operating-rate=fps
  max-bframes=0 prepend-sps-pps-to-idr-frames=1 vendor.qti-ext-enc-low-latency.enable=1
  bitrate-mode=1 (VBR)`.

### 3.5 Sistema, rede, Wi-Fi, USB, CPU

- `uname -r` 4.9.186 → sem `UDP_SEGMENT` (4.18+), sem `SO_TXTIME` (4.19+); `sendmmsg` ok.
  `/proc/sys/net/core/wmem_max` 16 MB, `tcp_wmem` 524288/1048576/4194304,
  `tcp_congestion_control = bic`, qdisc `pfifo_fast` em `wlan0`/`p2p0`.
- Wi-Fi: `dumpsys wifi` → `Freq=2412` (phone) e `Freq=5745` (AP tem 5 GHz), RSSI
  −43..−53, `PowerSave:true`, `SPF_POWER_SAVEMODE=true`, "Locks held:
  WifiLock{owc:wifi-ll type=4}, WifiLock{owc:wifi type=3}".
- CPU: 4× silver 300–1766 MHz + 4× gold 826–2803 MHz, `schedutil`; cpusets
  `foreground`/`top-app`/`camera-daemon` = 0-7, `background` = 0-3; `stune` boost 0;
  `/proc/<pid app>/limits`: **Max realtime priority 0** → `SCHED_FIFO` impossível;
  `CONFIG_RT_GROUP_SCHED=y`, `CONFIG_SCHED_TUNE=y`, `CONFIG_HIGH_RES_TIMERS=y`,
  `CONFIG_PREEMPT` (não VOLUNTARY/NONE).
- Threads do app (`/proc/<pid>/task/*/stat`): `owc-worker/owc-net/owc-jni-ev` nice −10,
  `HwBinder` −16 (callbacks do HAL de câmera no nosso processo), main −10, `RenderThread`
  −4. Provider de câmera (pid 873, cpuset `camera-daemon`) e `cameraserver`: threads em
  **nice 0, CFS** — nossa `owc-enc-out` a −16 pode preemptar o produtor em core
  disputado (irrelevante na prática, mas não suba mais).
- Térmico: `dumpsys thermalservice` status 0, CPU 32 °C, ~60 zonas (`camera-usr`,
  `mmss-usr`, `cpu*-step`…).
- USB: `sys.usb.config = mtp,conn_gadget,adb`, controlador `a600000.dwc3`;
  `current_speed` negado ao shell. Medir com `adb push` de 200 MB (≈30–40 MB/s = USB 2;
  >100 MB/s = USB 3).
- Interfaces: `wlan0`, `p2p0`, `swlan0` (hotspot), `rmnet_*`; nenhuma `rndis0`/`ncm0`
  ativa (tethering USB desligado).

### 3.6 adb forward (referência de protocolo)

O protocolo adb permite **um único `A_WRTE` em voo por stream** até o `A_OKAY`
(`packages/modules/adb/docs/dev/protocol.md`; o "delayed ACK"/`ADB_BURST_MODE` que
remove isso é experimental e exige adbd novo — não está no Android 10). Cada quadro
custa portanto ≥ 1 ida-e-volta USB + 2 relays em user-space (adbd no phone, adb server
no PC): consistente com os +1–5 ms medidos. É o teto do que `adb forward` pode dar;
`adb reverse` não muda o caminho de dados.

---

## 4. (d) Revisão do core nativo — o que custa latência/robustez

| Onde | Problema | Correção |
|---|---|---|
| `encoder/video_encoder.cpp:323-325` + `:283-291` + `net/sender.cpp:256` | AU copiado 3× (tmp → `EncodedFrame::au` → `tcpBuf_`) | 1 cópia direto do buffer do codec; `sendmsg` com `iovec[3]` |
| `encoder/encoder_options.cpp:192-193` | `latency=1` é no-op no OMX.qcom (ConfigLatency não suportada, §3.4); o comentário sugere efeito | manter (inócuo), anotar; a baixa latência real vem de `vendor.qti-ext-enc-low-latency.enable` — confirmar A/B |
| `encoder/encoder_options.cpp:195-196` | `operating-rate = fps` → Venus vota clock mínimo; encode ≈ 1 período | P4 |
| `params.cpp:104-106` + `encoder_options.cpp:168-173` | CBR nunca tentado no AVC por causa do MediaCodecList | P6 (tentar e verificar eco) |
| `net/sender.cpp:21-22,174-177,273-288` | pacer impõe 8 ms/quadro e ~37 ms/IDR a 8 Mbps | P5 |
| `net/sender.cpp:397-405` | throttle TCP por `SIOCOUTQ` com `wait_for(2 ms)` em polling | `TCP_NOTSENT_LOWAT` + `poll(POLLOUT)` |
| `net/socket_util.cpp:71-83` | TCP sem DSCP/`SO_PRIORITY` (vídeo TCP no Wi-Fi vai em AC_BE) | `setDscpEf(fd)` + `SO_PRIORITY 6` |
| `camera/camera_stream.cpp:42-55` | só lê `SENSOR_TIMESTAMP`; sem `PIPELINE_DEPTH`/`EXPOSURE_TIME` → impossível separar exposição de pipeline | P10 |
| `camera/camera_info.cpp:197-199`, `params.cpp:155-156` | `max_fps` truncado a `maxAeFps` (30) mesmo com MANUAL_SENSOR e minFrameDuration 16,7 ms | P1 |
| `camera/camera_controls.cpp:165-173` | só NR/edge; hotPixel/shading/tonemap/aberration/faceDetect/lensShadingMap/sceneMode ficam no default do template | P9 (tabela de knobs) |
| `hs/HighSpeedCamera.java:181-182` | `USAGE_GPU_SAMPLED_IMAGE` ativa o VSync hold Samsung na thread de callback do HAL | P2 |
| `hs/HighSpeedCamera.java:215-242` | `onCaptureCompleted` a 240 Hz via Binder/Java só para contar e medir (≈2–3 % de um core) | usar `onCaptureStarted` (mais barato) para contagem e amostrar o resultado 1 a cada 8 |
| `session.cpp:703-711` | fila HS = `cameraFps/30` = 8 quadros; lote real = 4 e o encoder só recebe `fps` → permite até ~130 ms de backlog em congestão | `q = max(maxQueue, ceil(batch × fps / hsFps))` com `batch` de `availableHighSpeedVideoConfigurations[4]` |
| `WebcamService.kt:179-206` | locks Wi-Fi sem as condições que os ativam (foreground+tela) em modo tela apagada; overlay não conta como foreground | P7 |
| `WebcamService.kt:343-361` | prefere overlay; Activity só como fallback | inverter em Wi-Fi |
| `util/thread.cpp:11-19` | `nice −16` no `owc-enc-out` vs HAL em nice 0 | −10 basta; nada de RT (limite 0) |
| `net/net_server.cpp:37-39` | UDP: `SO_SNDBUF` 1 MB + DSCP ok; sem `SO_PRIORITY` | adicionar |
| `camera/camera_stream.cpp:140` | `TEMPLATE_RECORD` (intent VIDEO_RECORD) pode ligar nodes "record" Samsung (`uniplugin_record`); PREVIEW deu igual | testar `NodeBypass=1` (P9) |
| `pipeline.cpp:96` | HS ignora preview local sem avisar o PC | enviar `ParamState`/log (robustez, não latência) |

---

## 5. (e) O que NÃO vale a pena (e por quê)

- **Slice mode / partial-frame delivery** (`vendor.qti-ext-enc-slice.spacing`,
  `AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME`): mesmo que o SDM845 entregue fatias (não
  confirmado; "slice delivery mode" saiu do venc após o 8996), o decoder MF do PC só
  decodifica AUs inteiras — o ganho fica limitado ao overlap transmissão/encode
  (< 2 ms a 100 Mbps) contra +5–10 % de bitrate.
- **SCHED_FIFO/RR, cgroups, `perf_lock`**: `Max realtime priority 0` no processo;
  `libqti-perfd-client.so` é vendor (namespace de linker bloqueia apps); `schedtune`
  não é gravável. Já estamos em nice −10/−16 e cpuset 0-7.
- **`Window.setSustainedPerformanceMode`**: *reduz* o clock máximo para evitar
  throttling; o contrário do que queremos. ADPF/PerformanceHint exige API 31.
- **120 fps HS via `samsung.android.scaler.availableHighSpeedVideoConfigurations`**:
  `createHighSpeedRequestList` valida contra a lista padrão (só 240) e o caminho
  oculto (`CaptureRequest.setPartOfCHSRequestList`) está sob restrição de API; o lote
  continuaria ≥ 2 (≥ 16,7 ms) → ganho ≈ 0 sobre 240 fps / lote 4.
- **AOA (acessório USB)**: exige driver WinUSB (Zadig/admin) no PC, derruba adb/MTP
  durante o modo acessório e é USB 2 bulk; ganho ~1–2 ms sobre RNDIS. `adb reverse`:
  mesmo relay.
- **UDP GSO (`UDP_SEGMENT`)/`SO_TXTIME`**: kernel 4.9 não tem; `sendmmsg` já cobre.
- **`max-fps-to-encoder` na sessão normal, `repeat-previous-frame-after`, B-frames,
  AV1**: já descartados pelo projeto; sem HW AV1 no SDM845.
- **Afinidade/clock de CPU agressivos**: nosso trabalho por quadro é de dezenas de µs;
  HAL e Venus (hardware) dominam. Afinidade só como polimento (P8).
- **Trocar template (PREVIEW/RECORD/ZSL)**: medido sem diferença; intent não muda o
  pipeline de vídeo do CamX além dos nodes Samsung (coberto por P9).
- **Zoom via GL/HWC**: `SCALER_CROP_REGION` é feito no IFE/IPE sem custo de latência;
  um passe GL adicionaria 1 vsync.
- **HEVC "por latência"**: Venus HEVC tem o mesmo desempenho (mesmos performance
  points); o ganho é só de bytes (P11), condicionado ao decoder HW do PC.

---

## 6. Referências

- Código OWC: `android/app/src/main/cpp/{camera,encoder,net}/*`, `session.cpp`,
  `params.cpp`, `hs/HighSpeedCamera.java`, `WebcamService.kt` (linhas citadas acima).
- Aparelho: `adb shell dumpsys media.camera`, `dumpsys wifi`, `dumpsys thermalservice`,
  `dumpsys usb`, `getprop`, `cat /vendor/etc/media_codecs_vendor.xml`,
  `media_codecs_performance.xml`, `/proc/<pid>/{limits,task/*/stat,cpuset}`,
  `/sys/devices/system/cpu/*`, `/dev/cpuset/*/cpus`, `/proc/config.gz`,
  `adb logcat -d` (tags `OWC`, `OWC.HS`, `ACodec`, `OMX-VENC`, `OMXNodeInstance`,
  `Camera3-Device`, `Camera3-OutputStream`, `Camera3-Stream`, `CamX`).
- AOSP: `android.control.availableHighSpeedVideoConfigurations` (5º campo =
  `batch_size_max`) — developer.android.com/reference/android/hardware/camera2/CameraCharacteristics;
  `CaptureRequest.Key(String, Class)` público desde API 29;
  `ACameraManager_getTagFromName` (`NdkCameraManager.h`, API 29);
  `AMediaCodec_setAsyncNotifyCallback` (API 28); `AMEDIACODEC_BUFFER_FLAG_PARTIAL_FRAME`;
  Wi-Fi low-latency: https://source.android.com/docs/core/connect/wifi-low-latency;
  adb protocol: https://android.googlesource.com/platform/packages/modules/adb/+/HEAD/docs/dev/protocol.md
  e `docs/dev/delayed_ack.md`.
- Qualcomm: QMediaExtensions (docs.qualcomm.com, 80-56386-10) — `KEY_SLICE_SPACING_SIZE`,
  LTR (`KEY_LTR_*`), initial-QP; `vendor.qti-ext-enc-low-latency.enable` [verificado no
  aparelho]. Semântica do `iso_exp_priority`: SnapdragonCamera `CaptureModule`
  [não verificado]. `msm_vidc` (kernel 4.9 `drivers/media/platform/msm/vidc`): operating
  rate → `msm_vidc_calc_freq`; low-latency → `VIDC_WORK_MODE_1` [não verificado].
