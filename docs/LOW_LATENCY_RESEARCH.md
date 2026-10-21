# OWC — Pesquisa de baixa latência (scrcpy, Moonlight, Sunshine, WebRTC)

Método: clones rasos lidos em `scratchpad/research/` (scrcpy, moonlight-common-c, moonlight-android,
moonlight-qt, Sunshine, HEAD de 2026-10). Referências no formato `repo:arquivo:linha|função`.
Itens marcados **[não verificado]** vêm de memória/AOSP/web e precisam de teste em aparelho real.

## 0. Licenças (ler primeiro)

| Projeto | Licença | Uso permitido p/ OWC |
|---|---|---|
| scrcpy | Apache-2.0 | Pode reutilizar trechos (manter NOTICE/atribuição). Mesmo assim, as ideias abaixo são triviais de reescrever. |
| moonlight-common-c, moonlight-android, moonlight-qt | GPL-3.0 | **Somente ideia. NÃO copiar código** (inclui `RtpVideoQueue.c`, `MediaCodecHelper.java`, SPS fixup do `ffmpeg.cpp`). |
| Sunshine | GPL-3.0 | **Somente ideia. NÃO copiar** (`stream.cpp`, `cbs.cpp`, `video.cpp`). |
| nanors (RS usado por moonlight/sunshine, submódulo) | **[não verificado]** — conferir LICENSE no repo `sleepybishop/nanors` antes de vendorizar; senão escrever RS GF(256) próprio (~150 linhas, Cauchy). |
| Chaves de MediaFormat (strings) | Fatos de API, não copyright | Livre. |

---

## 1. Resumo executivo — top 10 ações (ganho / esforço)

| # | Ação | Ganho | Esforço | Origem |
|---|---|---|---|---|
| 1 | Encoder: `latency=1`, `priority=0`, `repeat-previous-frame-after`, sem B-frames, Baseline/CBR, `max-fps-to-encoder` (tabela §2) | Alto (-1 a -3 frames de pipeline do encoder) | Baixo | scrcpy `SurfaceEncoder.createFormat` L312-335 |
| 2 | **Patch do SPS (VUI bitstream_restriction: `max_num_reorder_frames=0`, `max_dec_frame_buffering=1`, `num_ref_frames=1`)** no PC antes de entregar ao decoder MF/FFmpeg | Alto (elimina 1-N frames de espera no decoder DXVA; encoders Android quase nunca emitem VUI) | Médio (parser/escritor de SPS H.264 ~200 linhas) | moonlight-qt `ffmpeg.cpp:1763-1800` `writeBuffer`; Sunshine `cbs.cpp:138-190` `make_sps_h264` |
| 3 | Decoder MF: `MF_LOW_LATENCY=TRUE` + `CODECAPI_AVLowLatencyMode` + alimentar 1 AU e drenar imediatamente (sem esperar `NEED_INPUT` batch) | Alto | Baixo | moonlight-qt `ffmpeg.cpp:514` (`AV_CODEC_FLAG_LOW_DELAY`), `:532-536` (thread_count=1 em HW) |
| 4 | UDP: FEC Reed-Solomon por frame, 20 % default, mín. 2 parity, blocos ≤255 shards (§3.1) | Alto em Wi-Fi (perda 1-5 % comum) | Médio-alto | moonlight-common-c `RtpVideoQueue.c:711`; Sunshine `stream.cpp:1611-1660`, `fec::encode` L858 |
| 5 | Pacing: enviar fragmentos de um frame em rajadas de ≤64 KB, espaçadas por token bucket (~80 % do link por ms) em vez de rajada única; `SO_SNDBUF` ≥ 1 frame IDR | Médio-alto (evita drop em AP/buffer do roteador) | Médio | Sunshine `stream.cpp:1672-1830` |
| 6 | Perda → política de keyframe em 3 níveis (esperar FEC; RFI/IDR com rate-limit; IDR periódico de segurança) (§3.4) | Alto robustez | Baixo | moonlight-common-c `VideoDepacketizer.c:97-135`, `ControlStream.c:386-450` |
| 7 | Nunca ter fila no phone: escrita bloqueante + drop até IDR (já no design); no PC `frame_buffer` de 1 slot (sobrescreve) | Alto | Baixo | scrcpy `frame_buffer.c:47-62`; `Streamer.writePacket` |
| 8 | Bitrate adaptativo (GCC-like) usando `ReceiverReport` + `send_queue` (§3.5) com `video-bitrate` ao vivo | Alto robustez | Médio | draft-ietf-rmcat-gcc; WebRTC |
| 9 | Fallback de configure do encoder (escada de downsizing + remoção progressiva de chaves vendor) (§2.2) | Alto compat. | Baixo | scrcpy `SurfaceEncoder.prepareRetry` ; moonlight-android `setDecoderLowLatencyOptions` (tryNumber) |
| 10 | Intra-refresh (se `FEATURE_IntraRefresh`) como alternativa a IDR grande em UDP: suaviza picos de bitrate/fragmentos (§2.4) | Médio | Médio (JNI p/ detecção) | Sunshine `nvenc_base.cpp:329-338` (conceito); AOSP |

---

## 2. Android — encoder

### 2.1 Tabela de chaves MediaFormat

Chaves genéricas (string = valor literal que funciona também via NDK `AMediaFormat_setInt32/Int64/Float`, mesmo sem constante `AMEDIAFORMAT_KEY_*` no NDK compilado).

| Chave (string) | Valor | API | Vendor | Origem (arquivo:linha) |
|---|---|---|---|---|
| `mime` | `video/avc` / `video/hevc` | 16 | todos | scrcpy `SurfaceEncoder.java:312` |
| `color-format` | `0x7F000789` (`COLOR_FormatSurface`) | 18 | todos | `SurfaceEncoder.java:316` |
| `bitrate` | bps inicial (= ceiling do adaptativo) | 16 | todos | `SurfaceEncoder.java:313` |
| `frame-rate` | fps real da câmera (scrcpy põe 60 fixo pois fonte é variável; **nós temos fps fixo → usar o real**) | 16 | todos | `SurfaceEncoder.java:315` |
| `color-range` | `2` (LIMITED) | 24 | todos | `SurfaceEncoder.java:318` |
| `i-frame-interval` | `2` (s) com UDP; `10`+ (ou 0 = nunca) em TCP/USB. scrcpy usa 10 | 16 | todos | `SurfaceEncoder.java:320` (`DEFAULT_I_FRAME_INTERVAL=10`, L30) |
| `repeat-previous-frame-after` | `100000` (µs) | 19 | AOSP surface-encoder | `SurfaceEncoder.java:322` (`REPEAT_FRAME_DELAY_US`, L31). Ver §2.5 |
| `priority` | `0` (realtime) | 23 | todos | `SurfaceEncoder.java:325` |
| `latency` | `1` (saída assim que 1 frame entra). scrcpy seta a partir de API 26 (string, a constante pública é API 30) | 26 (string) / 30 (const) | AOSP C2 | `SurfaceEncoder.java:329` |
| `max-fps-to-encoder` | float = fps alvo (descarta excedente antes do encoder) | privada até 29; scrcpy usa em todas | AOSP | `SurfaceEncoder.java:33,335` |
| `bitrate-mode` | `2` = CBR (`BITRATE_MODE_CBR`); cair p/ `1` VBR se `configure` falhar | 21 | todos | (não usado pelo scrcpy; Sunshine usa CBR em todos os encoders: `video.cpp:790,1109`; `nvenc_base.cpp:260`) |
| `profile` / `level` | H.264: `AVCProfileBaseline` (=1) ou ConstrainedBaseline; level alto o bastante p/ res+fps. Baseline = sem B-frames, sem reordenação | 21 | todos | Sunshine `video.cpp:2055` (`max_b_frames=0`, "B-frames delay decoder output") |
| `max-bframes` | `0` (explícito, se aceitar; ignorar falha) | 29 | AOSP | idem, conceito |
| `prepend-sps-pps-to-idr-frames` | `1` (SPS/PPS em todo IDR → resync UDP sem depender do CSD) | 26 | AOSP | Sunshine `nvenc_base.cpp:361,392` (`repeatSPSPPS=1`), scrcpy re-mescla config em `packet_merger.c:19-54` |
| `intra-refresh-period` | N frames (ex. fps*1 a fps*2) **só se** `FEATURE_IntraRefresh` | 21 | alguns (Qualcomm/Exynos sim; MTK variável) | conceito Sunshine `nvenc_base.cpp:329-338` (`intraRefreshPeriod=300`); detecção §2.4 |
| `operating-rate` | `fps` (ou `Short.MAX_VALUE` só em Qualcomm *decoder*; para **encoder** preferir fps real, valor enorme pode falhar no configure) | 23 | Qualcomm | moonlight-android `MediaCodecHelper.java:482-495,520-535` (`decoderSupportsMaxOperatingRate`; ressalva de crash em Adreno 620) |
| `vendor.qti-ext-enc-low-latency.enable` | `1` | 26 (vendor ext.) | Qualcomm (`c2.qti.*`, `OMX.qcom.*`) | **[não verificado]** — contraparte do decoder `vendor.qti-ext-dec-low-latency.enable` (moonlight-android `MediaCodecHelper.java:564`). Não achei em fonte pública; Qualcomm documenta a família `vendor.qti-ext-enc-*` (ex.: `vendor.qti-ext-enc-initial-qp.qp-i-enable`) em docs.qualcomm.com "QMediaExtensions". Descobrir em runtime (abaixo) |
| `vendor.rtc-ext-enc-low-latency.enable` | `1` | 26 | Samsung Exynos (`c2.exynos.*`, `OMX.Exynos.*`) | **[não verificado]** — análoga a `vendor.rtc-ext-dec-low-latency.enable` (`MediaCodecHelper.java:222,580`) |
| `vdec-lowlatency` | — | — | MTK/Amlogic **decoder** | `MediaCodecHelper.java:528`. **Sem contraparte de encoder conhecida**; ignorar no encoder |
| `vendor.hisi-ext-low-latency-video-dec.*` | — | — | Kirin **decoder** | `MediaCodecHelper.java:572-573`. Sem contraparte encoder conhecida |
| `vendor.low-latency.enable` | — | — | Amlogic **decoder** | `MediaCodecHelper.java:588` |
| `low-latency` (`KEY_LOW_LATENCY`) | `1` — **decoder-only** (API 30); encoders usam `latency` | 30 | AOSP | `MediaCodecHelper.java:503` |

Resumo das chaves vendor de **decoder** extraídas de moonlight-android (`MediaCodecHelper.java`): `vendor.qti-ext-dec-low-latency.enable` (L564), `vendor.qti-ext-dec-picture-order.enable` (L560), `vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req/-rdy` (L572-573), `vendor.rtc-ext-dec-low-latency.enable` (L580), `vendor.low-latency.enable` (L588), `vdec-lowlatency` (L528), `low-latency` (L503), `operating-rate` (L520-535), `priority` (L539). Lista de "known vendor options": L220-223.

**Descoberta em runtime de chaves vendor de encoder (recomendado em vez de hardcode):** Java `MediaCodec.getSupportedVendorParameters()` / `getParameterDescriptor(name)` (API 31) — enumerar, filtrar por `low-latency`/`lowlatency`/`ext-enc` e aplicar as que existirem (tipo int). **Não existe no NDK** → precisa de JNI/Kotlin chamando `MediaCodec` Java só para sondar (criar o codec por nome, ler lista, `release()`), depois configurar via NDK. Em API < 31 usar a lista hardcoded acima, aplicada com a escada de fallback (§2.2), nunca como requisito.

Boa prática do moonlight-android (ideia): moonlight aplica opções "da mais arriscada para a menos arriscada" e, a cada falha de `configure`, tenta de novo com `tryNumber+1` removendo as chaves mais exóticas (`setDecoderLowLatencyOptions(format, info, tryNumber)`, `MediaCodecHelper.java:497-595`). Copiar a *estratégia* para o encoder.

### 2.2 Falha de configure — estratégia de fallback

scrcpy (`SurfaceEncoder.streamCapture` + `prepareRetry`, Apache-2.0):
1. Captura todo `IllegalStateException|IllegalArgumentException|IOException` ao redor de `configure → createInputSurface → capture.start → start`.
2. `prepareRetry`: se já enviou frame, até `MAX_CONSECUTIVE_ERRORS=3` retries com `sleep(50ms)`. Antes do 1º frame: (a) aplica `VideoCapabilities` do encoder (alinhamento width/height, faixa suportada) e tenta; (b) senão desce `MAX_SIZE_FALLBACK = {2560,1920,1600,1280,1024,800}` (`chooseMaxSizeFallback`) retentando.
3. Entre tentativas: `mediaCodec.stop()` (try/catch), `reset()`, `surface.release()`.
4. Detalhe: encoder thread precisa de `Looper.prepare()` (Meizu travam) — `SurfaceEncoder.start`, issue scrcpy #4143.

Proposta OWC (ordem de degradação, cada passo = novo `AMediaCodec_configure`):
1. Formato completo (tabela §2.1 + vendor).
2. Remover chaves vendor `vendor.*`.
3. Remover `max-fps-to-encoder`, `latency`, `max-bframes`, `intra-refresh-period`, `prepend-sps-pps-to-idr-frames`.
4. `bitrate-mode` CBR → VBR; profile Baseline → deixar default (sem key).
5. Reduzir resolução/fps conforme lista da câmera (largura/altura alinhadas ao `getWidthAlignment` — só via JNI `VideoCapabilities`; no NDK use tentativa/erro com múltiplos de 16).
6. Trocar de encoder: H.264 → outro encoder por nome (`AMediaCodec_createCodecByName`); evitar `c2.android.avc.encoder`/`OMX.google.*` (SW) exceto como último recurso.
Em cada falha logar via `MsgType::Log` e notificar PC com novo `VideoFormat` (epoch++).

### 2.3 PTS

- scrcpy repassa `bufferInfo.presentationTimeUs` direto como PTS do pacote (`Streamer.writePacket`), bit 63 = session, 62 = config, 61 = keyframe, resto = PTS (`Streamer.java:15-17,100-125`; parse em `app/src/demuxer.c:139-178`). Pacote de config (`BUFFER_FLAG_CODEC_CONFIG`) é guardado e **fundido no próximo pacote de mídia** (`packet_merger.c`) para que SPS/PPS e o IDR cheguem como 1 AU.
- Para OWC: o PTS que a câmera dá à surface = `SENSOR_TIMESTAMP` (CLOCK_MONOTONIC ou REALTIME conforme `SENSOR_INFO_TIMESTAMP_SOURCE`). O encoder preserva em `presentationTimeUs`. **Usar esse valor como `capture_ts_us`** (já previsto no protocolo: "phone CLOCK_MONOTONIC"). Se `TIMESTAMP_SOURCE == REALTIME`, converter com offset (`elapsedRealtimeNanos` vs `uptimeNanos`) uma vez no start.
- Não confiar em ordem de saída ≠ ordem de PTS (sem B-frames são iguais). Gerar `frame_seq` próprio no momento do `dequeueOutputBuffer`.
- Latência de encode = `now_monotonic - pts` na saída (já no `Stats::encode_latency_ms`).

### 2.4 Intra-refresh e detecção

- API: `MediaCodecInfo.CodecCapabilities.isFeatureSupported(FEATURE_IntraRefresh)` (`"intra-refresh"`, API 21) e chave `intra-refresh-period`. **O NDK não tem `MediaCodecList`/`CodecCapabilities` (nem `isFeatureSupported`)** → JNI/Kotlin: `MediaCodecList(ALL_CODECS).codecInfos` → filtrar `isEncoder` + mime → `getCapabilitiesForType(mime).isFeatureSupported(...)`; também `getVideoCapabilities()` (alinhamento, `getSupportedFrameRatesFor`, `getBitrateRange`, `getSupportedPerformancePoints` API 29), `isHardwareAccelerated` (API 29), `FEATURE_LowLatency` (API 30). Passar o resultado ao nativo (`jint flags`) antes de configurar.
- Quando usar: UDP com perda; IDR de 150 KB = ~110 fragmentos em rajada causa perda correlacionada. Intra-refresh mantém frames de tamanho uniforme e dispensa IDR (receptor recupera em `period` frames). Ainda assim manter `request-sync` como fallback. Sunshine só habilita sob pedido do cliente e só NVENC (`nvenc_base.cpp:329-338`; `stream.cpp:121` tipo de frame "P com intra-refresh blocks").
- Armadilha: com intra-refresh o decoder do PC precisa de `AV_CODEC_FLAG_OUTPUT_CORRUPT`/equivalente para exibir frames parcialmente limpos (moonlight-qt `ffmpeg.cpp:517-518`). No MF isso é natural (não descarta); mas o `FrameAssembler` hoje descarta deltas até IDR após perda → com intra-refresh deve **continuar entregando** e só pedir sync se `period` frames passarem sem recuperação.

### 2.5 `repeat-previous-frame-after` com câmera

scrcpy usa para tela (que não produz frames quando estática): repete o último frame após 100 ms para "display the very first frame, and recover from bad quality when no new frames" (`SurfaceEncoder.java:321-322`). Para **câmera** (frames chegam a 30 fps sempre): irrelevante em regime; útil apenas (a) logo após `start` (garantir 1º frame/IDR se a câmera demorar), (b) cena escura com AE em baixo fps (ex. 15 fps com AE target range variável) — mantém stream vivo para o watchdog do PC. Risco: em alguns encoders a repetição entra como frame extra e aumenta bitrate; use `1_000_000/fps*3` (~100 ms @30) como scrcpy. **Recomendação:** manter 100 ms, mas **não** se o `max-fps-to-encoder` estiver ativo (interação indefinida) — testar. Se causar duplicatas visíveis, remover (impacto baixo para câmera).

### 2.6 Bitrate ao vivo e request-sync

- scrcpy **não** implementa nenhum dos dois (grep `setParameters|PARAMETER_KEY|REQUEST_SYNC` em `server/src` vazio).
- NDK: `AMediaCodec_setParameters(codec, AMediaFormat*)` (API 26) com `AMediaFormat_setInt32(f, "video-bitrate", bps)` e `AMediaFormat_setInt32(f, "request-sync", 0)` (constantes Java `PARAMETER_KEY_VIDEO_BITRATE`, `PARAMETER_KEY_REQUEST_SYNC_FRAME`). Chamar da thread de controle; é thread-safe pela doc.
- `request-sync` não é instantâneo: o IDR sai 1-3 frames depois; **ignorar pedidos repetidos por ≥ 1 RTT+2 frames** (rate-limit no phone também, não só no PC).
- Alguns encoders (Exynos antigos) ignoram `video-bitrate` em CBR; validar medindo `Stats::bitrate_bps` e, se não convergir em 2 s, reconfigurar (epoch++) como último recurso.
- Fonte de frames de câmera pode também forçar IDR trocando `bitrate`+`request-sync` juntos.

---

## 3. Transporte UDP

### 3.1 FEC recomendado para o nosso formato

O que Moonlight/Sunshine fazem (idéia, GPL):
- FEC **por frame**, Reed-Solomon sistemático GF(2^8) (nanors), shards de tamanho fixo `blocksize = packetSize + RTP_HDR`. Paridade: `parity = ceil(data * pct / 100)` (`RtpVideoQueue.c:711` `(bufferDataPackets*fecPercentage+99)/100`; `Sunshine stream.cpp:865`). Mínimo de parity: cliente pede `minRequiredFecPackets=2` (`moonlight-common-c SdpGenerator.c:211`), servidor sobe o % do frame para atingi-lo (`stream.cpp:867-872`).
- Limite 255 shards/bloco → frame dividido em **até 4 blocos FEC** (2 bits): `max_data_shards_per_fec_block = 255*100/(100+pct)` (`stream.cpp:1630`); acima de 4 blocos desliga FEC para o frame (`stream.cpp:1638-1641`). Cabeçalho do pacote leva `fecInfo` (índice do shard 10 bits, % 8 bits, nº data shards 10 bits) e `multiFecBlocks` (`Video.h:30-34`, `RtpVideoQueue.c:25,150-156`).
- Receptor: recupera quando `received >= data_shards` (`reconstructFrame`, `RtpVideoQueue.c:~185-370`); preenche o resto do shard com zero (Annex-B tolera zeros finais — comentário em L~345); valida SOF/EOF do shard recuperado.
- Se `missing > parity` já no meio do frame, antecipa notificação de perda sem esperar timeout (`RtpVideoQueue.c:215-223`).

**Proposta OWC (cabe em `UdpType::Fec` + header de 32 B, sem mudar a versão do header):**

- Shards de dados = os fragmentos `Video` já existentes, cada um com payload ≤ `kUdpMaxPayload` (1368); **o último fragmento é preenchido com zeros virtualmente** até 1368 para o cálculo RS (o comprimento real vem de `frame_size`). Sem mudar fragmentação atual.
- Bloco FEC: `k ≤ 200/(1+pct/100)`-ish; usar a fórmula Sunshine `kmax = 255*100/(100+pct)` (pct=20 → kmax=212). Frames com `frag_count > kmax` são divididos em `nblocks = ceil(frag_count / kmax)` blocos de tamanho ~igual (limite 8; acima disso FEC off no frame). Bloco `b` cobre fragmentos `[b*kb, min((b+1)*kb, frag_count))` com `kb = ceil(frag_count/nblocks)` — receptor deriva sem campo extra.
- Datagrama `Fec`: header de 32 B com `type=Fec`, `frame_seq`, `epoch`, `capture_ts_us`, `frame_size`, `flags` copiados do frame; `frag_count` = nº de fragmentos de dados do frame; `frag_idx` = índice de parity global `p` (0..M-1); byte `reserved` = `M` (parity total do frame, <=255). Payload = exatamente 1368 B de parity (shard RS inteiro, sem metadados extras, para manter alinhamento de shard).
- Layout de blocos **derivado**, sem campo extra: funcao unica `fecLayout(frag_count, M)` em `protocol.h` (emissor e receptor usam a mesma): `nblocks = ceil(M / mmax)` com `mmax` tal que `kb + mb <= 255`; `kb = ceil(frag_count/nblocks)`, `mb = ceil(M/nblocks)`; bloco `i` cobre fragmentos `[i*kb, ...)` e parity `[i*mb, ...)`. Cobrir com testes de round-trip em `protocol/` (perda aleatoria 0-30 % e rajadas).
- Recuperação: bloco com `recebidos(dados+parity) ≥ kb` → decodifica; entrega o frame quando todos os blocos completos. Se faltar → mesmo caminho atual de "frame perdido" (§3.4).
- **Quando k é pequeno (P-frames de 1-3 fragmentos, o caso comum a 1080p/30 em 4-8 Mbps ≈ 17-33 KB = 13-25 fragmentos; a 720p muitas vezes 3-8):** `M = max(2, ceil(k*pct/100))` (≥2 como no moonlight). Para `k=1`, RS com M≥1 equivale a cópias — OK, sem tratamento especial. XOR de paridade única só cobre 1 perda e não resiste a rajada; **recomendo RS desde o início** (a implementação Cauchy GF(256) é pequena; custo: k=25,M=5 encode ~ 25*5*1368 mulacc ≈ 171 K ops, desprezível em ARM). XOR só como modo de contingência para CPUs fracas (não necessário).
- Custo de latência do FEC: encode no phone ocorre depois que o frame inteiro saiu do encoder (já esperamos o AU completo) → +<0,3 ms. Decode no PC só quando há perda.

**% default:** 20 % (Moonlight/Sunshine default: `config.cpp:1828` aceita 0-100; valor padrão do projeto Sunshine = 20, **[não verificado no código, doc do Sunshine]**), faixa adaptativa 10-50 % ajustada por perda medida (§3.5). Para USB/TCP: 0.

### 3.2 Pacing

- Sunshine (`stream.cpp:1672-1830`): envia em lotes ≤ `64 KiB / blocksize` pacotes (máx. 64; limite Windows: lotes >64 KiB "bypassam SO_SNDBUF", comentário L1675-1678) via `sendmmsg`/`WSASendMsg` batching, e entre lotes faz sleep para respeitar `ratecontrol_packets_in_1ms = 80 % × 1 Gbps / blocksize / 8` (L1673) — i.e., pacing "no limite do link LAN" apenas para impedir rajadas de line-rate que estouram buffer do switch/AP; preserva o tempo-de-início de frame (`ratecontrol_next_frame_start`).
- **OWC:** (a) agrupar em lotes de ≤ ~40 datagramas (≈ 55 KB) e `sendmmsg` (Android bionic API 21+), (b) pacing por token bucket com taxa = `max(20 Mbps, 4 × bitrate_atual)` (permite frame de 30 KB sair em ~12 ms em vez de 33 ms de pacing total, mas evita rajada de 150 KB instantânea de IDR), (c) FEC datagramas por último dentro do frame (se pacing atrasar, os dados chegam antes). (d) `SO_SNDBUF` UDP = 256-512 KB (≥ 1 IDR). Receptor Windows: `SO_RCVBUF` grande — moonlight usa `RTP_RECV_PACKETS_BUFFERED=2048 × (packetSize+16)` ≈ 2,9 MB (`VideoStream.c:35,331-332`); usar **≥ 2 MB** + thread de recepção dedicada com prioridade alta que só drena o socket (nenhum trabalho de depacketize no mesmo loop). Buffer grande ≠ latência: serve para absorver rajadas; a política "descartar frame mais velho" fica no assembler.
- DSCP EF (46) já no design; Sunshine usa `IP_TOS` dscp 40/48 conforme tipo (`platform/linux/misc.cpp:1061-1100`); em Android `setsockopt(IPPROTO_IP, IP_TOS, 0xB8)`; muitos APs domésticos ignoram, mas WMM mapeia para AC_VI/AC_VO.

### 3.3 Dados sobre tamanho de pacote
Moonlight usa 1392 de payload por padrão e 1024 em VPN (`moonlight-qt session.cpp:1664-1679`). Nosso 1400 total (1368 payload) é conservador e OK para Wi-Fi/Ethernet; manter configurável (ex. 1200 em hotspots com MTU reduzido).

### 3.4 Perda → keyframe: política

Moonlight (`moonlight-common-c`):
- `RtpVideoQueue.c`: perda detectada quando um frame novo chega com o anterior incompleto/irrecuperável (`~L620-690`); `notifyFrameLost(frame, speculative)`.
- `ControlStream.c:386-412` `queueFrameInvalidationTuple`: se RFI (reference frame invalidation) suportado, enfileira intervalo `[start,end]` (fila limitada; se estourar → IDR); senão **`LiRequestIdrFrame()`**. `LiRequestIdrFrame` (L415) descarta RFIs pendentes (IDR supera) e levanta evento.
- `VideoDepacketizer.c:97-135 dropFrameState`: sem RFI → `waitingForIdrFrame`; contador `consecutiveFrameDrops` com `CONSECUTIVE_DROP_LIMIT=120` (L30) → pede IDR (rede muito ruim, não spammar). Frame corrompido detectado por `streamPacketIndex` descontínuo (L~790) ⇒ drop + (IDR ou RFI).
- Loss stats periódicos: thread a cada **50 ms** (`LOSS_REPORT_INTERVAL_MS`, `ControlStream.c:297,1456-1481`) informa último frame bom — servidor só loga em Sunshine (`stream.cpp:1198-1204`). Sunshine reage a `IDX_REQUEST_IDR_FRAME` com `idr_events->raise` (`stream.cpp:1206-1209`) e a `IDX_INVALIDATE_REF_FRAMES` (L1212-1222).

**Proposta OWC (mobile H.264 Android não suporta RFI/LTR no NDK de forma confiável → apenas IDR):**
1. FEC primeiro: não pedir nada se o frame foi recuperado.
2. Frame irrecuperável: `FrameAssembler` marca `needKeyframe`; PC envia `Command{RequestKeyframe}` **imediatamente na 1ª ocorrência**, depois rate-limit **≥ max(250 ms, 2×RTT+2 frames)**; reenviar o pedido a cada 500 ms enquanto continuar `needKeyframe` (pedido pode se perder em UDP; hoje ele vai por TCP de controle → confiável, então 1 reenvio por 1 s basta).
3. Phone: ignora `RequestKeyframe` se um IDR foi emitido < 150 ms atrás (evita tempestade de IDR que piora a perda — IDR é 5-10× maior). Se `pct` FEC < 30 %, subir temporariamente o FEC no(s) próximo(s) IDR (+10 pts).
4. Após N=5 IDRs pedidos em 10 s → baixar bitrate 30 % e subir FEC (rede ruim); equivalente ao `CONSECUTIVE_DROP_LIMIT` para evitar loop.
5. IDR periódico de segurança: `i-frame-interval=2 s` em UDP (cura estado de decoder divergente por perda silenciosa). Em TCP/USB: 10 s+.
6. Se `FEATURE_IntraRefresh`: política da §2.4.
7. moonlight-qt usa `AV_EF_EXPLODE` + `OUTPUT_CORRUPT` para detectar erro de decode e pedir keyframe (`ffmpeg.cpp:517-526`): no MF, se `ProcessOutput` retornar erro/`MF_E_TRANSFORM_STREAM_CHANGE` repetido → também pedir IDR.

### 3.5 Bitrate adaptativo — pseudocódigo

Base: GCC (draft-ietf-rmcat-gcc): controlador por atraso (trendline/Kalman) + controlador por perda: **perda <2 % → aumenta ~5 %/s; 2-10 % → mantém; >10 % → `rate *= (1 − 0.5·loss)`**; AIMD do atraso: decreasing `β=0.85` sobre a taxa recebida medida; aumento multiplicativo até ~1,08/s longe da convergência, aditivo perto. (Valores do draft/WebRTC — confirmados em geral, constantes exatas **[não verificado nesta pesquisa]**; tratar como pontos de partida e calibrar.)

Entradas por `ReceiverReport` (≈4 Hz, 250 ms), deltas dos contadores cumulativos; `send_queue` do phone; `Stats.bitrate_bps`.

```
// constantes
MIN_BR = 500 kbps ; MAX_BR = Bitrate_param (ceiling) ; START = 0.7*MAX
HOLD_AFTER_DECREASE = 1500 ms ; INCREASE_PERIOD = 1000 ms (só aumenta 1x/s)
LOSS_LOW = 0.02 ; LOSS_HIGH = 0.10
GRAD_OVERUSE = +6 ms (delay_gradient) ; GRAD_UNDERUSE = -3 ms
JITTER_HIGH = 15 ms (aviso, não age sozinho)

on_report(rr):            // chamado ~4 Hz no phone
  dfl  = rr.fragments_lost  - prev.fragments_lost
  dfr  = rr.fragments_received - prev.fragments_received
  loss = dfl / max(1, dfl + dfr)                      // por fragmento (pré-FEC)
  fframes_lost = rr.frames_lost - prev.frames_lost    // pós-FEC: perda "visível"
  loss_s = ewma(loss_s, loss, 0.4)
  g      = rr.delay_gradient_ms
  q      = send_queue_len                              // frames no phone
  recv   = rr.recv_bitrate_bps

  state = NORMAL
  if g > GRAD_OVERUSE or q >= 2 or rr.decode_ms > 0.8*frame_interval_ms: state = OVERUSE
  elif g < GRAD_UNDERUSE and q == 0:                    state = UNDERUSE

  if fframes_lost > 0 or loss_s > LOSS_HIGH:            // perda real
      target = min(cur, recv) * (1 - 0.5*min(loss_s, 0.3))   // GCC loss-based
      target = min(target, 0.85*recv) if recv>0
      decrease(target); request_idr_policy()
  elif state == OVERUSE:
      decrease(0.85 * max(recv, 0.5*cur))               // AIMD beta=0.85 sobre taxa medida
  elif loss_s < LOSS_LOW and state != OVERUSE and (now - last_dec) > HOLD_AFTER_DECREASE
       and (now - last_inc) > INCREASE_PERIOD:
      step = (probing_far_from_last_good) ? cur*0.08 : 50 kbps   // multiplicativo longe, aditivo perto
      increase(min(MAX_BR, cur + step))
  // FEC adaptativo
  fec_pct = clamp(round(10 + 150*loss_s), 10, 50)      // ex: 2% perda -> 13%, 10% -> 25%
  if loss_s < 0.002 for 10 s: fec_pct = 10
  // regra de seguranca no phone, independente do PC (quando o RR atrasa >1 s):
  if q >= 2: cur = max(MIN_BR, cur*0.8); drop_until_idr()     // ja no design (fila <=2)

decrease(t): cur = max(MIN_BR, t); set video-bitrate(cur); last_dec = now
increase(t): cur = t; set video-bitrate(cur); last_inc = now
```
Notas: (1) a taxa total no fio = bitrate × (1+fec/100); descontar do alvo: `video_bitrate = target/(1+fec_pct/100)`. (2) Não aumentar enquanto `needKeyframe` ativo no PC. (3) `delay_gradient_ms` deve ser calculado no PC como inclinação (regressão linear em janela de ~20 frames) de `(arrival_i - capture_ts_i)` descontando offset do Ping/Pong — sensível a jitter de Wi-Fi power-save; por isso exigir 2 relatórios consecutivos em OVERUSE antes de agir. (4) RIST/SRT (ARQ): NACK + retransmissão dentro de um orçamento de latência (SRT `latency` ≥ ~2.5-4×RTT); só vale para RTT baixo — em LAN (RTT 2-5 ms) um NACK de **1 retransmissão** dentro de ~15 ms é viável e complementa FEC; **sugestão futura (v1.2)**: `UdpType::Nack` PC→phone com bitmap de fragmentos faltantes, phone mantém os últimos ~2 frames enviados em RAM. Não necessário na v1.

---

## 4. TCP / USB

- Nagle: scrcpy só liga `TCP_NODELAY` no socket de **controle** (`app/src/server.c:712`, `util/net.c:272-283`); o vídeo vai por socket local do adb (stream, sem Nagle útil). **OWC: `TCP_NODELAY` nos dois lados em todos os sockets** (vídeo no PC→phone via adb forward termina em `127.0.0.1` do PC; o `adbd` é quem fala com o phone: Nagle local no loop é inofensivo, mas ligar no socket do PC é de graça). No phone (servidor TCP no modo Wi-Fi) ligar também.
- Backpressure no scrcpy: **nenhuma fila**. A thread do encoder faz `IO.writeFully(fd, buffer)` bloqueante (`Streamer.writePacket`) dentro do loop `dequeueOutputBuffer` (`SurfaceEncoder.encode`). Se o socket enche, a thread bloqueia, os buffers de saída do encoder não são liberados, o encoder fica sem buffers e o `BufferQueue` da surface descarta frames antigos de entrada (a surface é produtor-consumidor com "drop oldest"). Isso limita latência automaticamente. No PC o `frame_buffer` tem **1 slot** e sobrescreve o frame pendente (`frame_buffer.c:47-62` `push` troca `pending_frame`; `consume` marca consumido) → decoder nunca fica atrás do renderer.
- **OWC (TCP/USB):** como temos fila ≤2 frames e drop-até-IDR, implementar com `SO_SNDBUF` pequeno (≈ 2× frame médio, 64-256 KB; Android pode dobrar o valor), medir fila = bytes não enviados (`ioctl SIOCOUTQ`/`TIOCOUTQ` no Linux/Android) em vez de só contagem de frames: se `SIOCOUTQ > 1.5 × frame_médio` → pular/descartar. Atenção: com TCP, descartar frames **P** quebra a cadeia → ao descartar tem de descartar até o próximo IDR e pedir IDR (já no design); alternativa melhor em USB: **não descartar, reduzir bitrate** (adaptativo §3.5 usando só `send_queue`).
- Estabelecimento do túnel (scrcpy `adb_tunnel.c:127-143`, `server.c:491-505`): tenta `adb reverse` (servidor no PC) e cai para `adb forward` se falhar (ex. `adb connect`). OWC usa `forward` (phone = servidor), compatível. Detalhe aproveitável: **ler 1 byte após conectar** para detectar que o servidor atrás do túnel está de fato escutando (`connect_and_read_byte`, `server.c:491-506`) — `adb forward` aceita a conexão TCP mesmo se nada escuta no phone; sem isso o `Hello` parece "pendurar". Proposta: phone envia imediatamente 1 byte (ou o `HelloAck` vem primeiro) e PC faz retry (scrcpy: N tentativas com delay) até receber. Servidor scrcpy usa `LocalServerSocket` abstrato (`DesktopConnection.java:60-100`); OWC usa porta TCP 47800 (já definido).
- Descoberta de adb: já no design.

---

## 5. Windows — decodificação e apresentação

### 5.1 O que moonlight-qt faz (FFmpeg + D3D11VA)
- `ffmpeg.cpp:514` `flags |= AV_CODEC_FLAG_LOW_DELAY`; `:517-518` `AV_CODEC_FLAG_OUTPUT_CORRUPT` + `flags2 |= AV_CODEC_FLAG2_SHOW_ALL` (mostrar frames com referências faltando); `:526` `err_recognition = AV_EF_EXPLODE` (erro de decode → pede IDR); `:532-536` **thread_count=1 em decode HW**, slice-threading só em SW.
- **SPS fixup** (`ffmpeg.cpp:1763-1800` `writeBuffer`, ativado quando o renderer não suporta RFI AVC, L~718-723): reescreve o SPS H.264 com `num_ref_frames=1`, `vui.max_dec_frame_buffering=1`, e, se faltar, cria `bitstream_restriction_flag=1` com `num_reorder_frames=0`, `max_bytes_per_pic_denom=2`, `max_bits_per_mb_denom=1`, `log2_max_mv_length_*=16`, `motion_vectors_over_pic_boundaries_flag=1` (comentário: NVENC novo não emite mais bitstream_restriction). Motivo: decoders HW (DXVA/VideoToolbox/Pi) seguram frames até `max_dec_frame_buffering`/DPB cheio quando não há restrição → latência de vários frames.
- Sunshine faz o mesmo no servidor: `cbs.cpp:138-190` `make_sps_h264` (`bitstream_restriction_flag=1`, `max_num_reorder_frames=0`, `max_dec_frame_buffering=max_num_ref_frames`, `low_delay_hrd_flag`), e HEVC em `make_sps_hevc` L197+.
- D3D11 apresentação (`ffmpeg-renderers/d3d11va.cpp:551-602`): swapchain `FLIP_DISCARD`, `BufferCount=3+1+1`, **SyncInterval 0 + `DXGI_PRESENT_ALLOW_TEARING`** quando suportado (L790-827); **não** usar `SetMaximumFrameLatency(1)` (faz o Present(0) bloquear no DWM, L567-575). Pool de textura de decode `+3` extra frames (L739-741).
- Pacer (`ffmpeg-renderers/pacer/pacer.cpp`): fila de render máx. 3 (`MAX_QUEUED_FRAMES`, L21); `dropFrameForEnqueue` descarta o mais velho (L400-406); sem vsync, enfileira direto (L416-424); com pacing, `frameDropTarget` de 1..3 conforme histórico (L359-395). Ou seja: **sempre apresentar o mais novo, descartar os antigos**.

### 5.2 Media Foundation vs FFmpeg d3d11va
- moonlight usa FFmpeg+D3D11VA porque é multiplataforma e dá controle fino (flags acima, `AV_EF_EXPLODE`, hw_frames_ctx, pool). Latência de decode em si é equivalente (ambos DXVA2/D3D11 video decoder do driver); a diferença está nos **buffers internos**: o MFT H.264 da Microsoft tem comportamento de reordenação/DPB dependente do SPS e do atributo low-latency.
- **OWC (decisão):** manter MF (sem DLL FFmpeg LGPL/GPL, binário único), **com**: `MF_LOW_LATENCY=TRUE` via `IMFAttributes` do MFT (Win8+) e `ICodecAPI::SetValue(CODECAPI_AVLowLatencyMode, TRUE)` **antes** de definir tipos de mídia; `MFT_MESSAGE_SET_D3D_MANAGER` com `IMFDXGIDeviceManager` (`MF_SA_D3D11_AWARE`) para saída em textura NV12 sem cópia; `CODECAPI_AVDecNumWorkerThreads=1`?? **[não verificado]** (equivalente do thread_count=1; DXVA não usa workers de CPU de qualquer forma); entrada: 1 AU por `IMFSample` com timestamp, chamar `ProcessOutput` em loop logo após cada `ProcessInput` (não esperar acumular); tratar `MF_E_TRANSFORM_NEED_MORE_INPUT` como "sem frame" e **`MF_E_TRANSFORM_STREAM_CHANGE`** (renegociar tipo de saída). Manter fallback para FFmpeg d3d11va (opcional, plugin) se o MFT do driver tiver DPB grande, já que lá dá para forçar `flags|=LOW_DELAY` + `thread_count=1`.
- Se medir > 1 frame de atraso no decoder MF mesmo com low-latency → aplicar o **SPS patch** (§5.3). Medir: `capture_ts` → `ProcessOutput` e comparar com intervalo de frame.

### 5.3 Encoder Android emite VUI/bitstream_restriction? Precisamos de patch?
- **Não verificado em aparelho.** Por conhecimento: encoders HW Android (Qualcomm/Exynos/MTK) em Baseline normalmente emitem SPS **sem VUI** (ou com VUI só de timing/cor); `num_ref_frames` costuma ser 1-4; sem `bitstream_restriction` o decoder assume `max_dec_frame_buffering` = MaxDpbFrames do nível (pode ser 4-16) e **o decoder MF/DXVA pode bufferizar** até esse valor antes de emitir (comportamento observado em decoders HW; é exatamente o problema que moonlight-qt corrige).
- **Recomendação: implementar `patchH264Sps()` no PC (no `FrameAssembler`/ao receber `VideoFormat.csd` e em todo SPS de IDR)**: parse Exp-Golomb do SPS (remover emulation prevention, ler até VUI), setar `vui_parameters_present_flag=1`, `bitstream_restriction_flag=1`, `max_num_reorder_frames=0`, `max_dec_frame_buffering=num_ref_frames` (ou 1), `num_ref_frames=1` **só se o stream realmente usa 1 ref** (Baseline do Android de baixa latência normalmente usa 1; se o encoder usar mais refs, forçar 1 corrompe a decodificação → então manter `num_ref_frames` original e só ajustar `max_dec_frame_buffering=num_ref_frames`), reescrever com emulation prevention. Guardar o SPS original para fallback. HEVC: `sps_max_num_reorder_pics[]=0`, `sps_max_latency_increase_plus1=0`, `sps_max_dec_pic_buffering_minus1=num_refs` (Sunshine `cbs.cpp:197-260` como referência de campos).
- **Alternativa no phone:** reescrever o SPS no `CSD` e no SPS em-banda do IDR antes de transmitir (mesmo código, roda no phone; vantagem: PC recebe stream já "bom", útil para o decoder do OBS/outros). Preferir **no PC** (um só lugar, atualiza sem reinstalar APK) — ou nos dois via lib `protocol/` compartilhada (C++17 portátil).
- Como validar: logar `num_ref_frames` e presença de VUI do CSD de 5+ aparelhos; teste A/B de latência (LED/relógio) com e sem patch.

### 5.4 Apresentação do frame mais novo
- Decoder thread → `ShmWriter` sobrescreve (1 slot lógico + double buffer para evitar tearing de leitura); consumidores leem o último. Equivalente ao `sc_frame_buffer` do scrcpy (Apache) e ao `dropFrameForEnqueue` do moonlight.
- No preview D3D11 do OWC.exe: copiar o padrão `FLIP_DISCARD` + `Present(0, ALLOW_TEARING)` + **sem** `SetMaximumFrameLatency(1)`; usar `BufferCount ≥ 4-5` (L577) — AMD trava com 3.
- OBS: `obs_source_set_async_unbuffered` já no design.

---

## 6. Notas rápidas externas

- **WebRTC GCC** (draft-ietf-rmcat-gcc-02, Holmer): delay-based (filtro de tendência/Kalman sobre atraso de chegada inter-grupo → estados Overuse/Normal/Underuse com limiar adaptativo) + loss-based no remetente; AIMD com β≈0,85 sobre a taxa recebida. Aplicado em §3.5.
- **SRT/RIST**: ARQ com orçamento de latência (`latency` ≥ ~2-4× RTT; NACK periódico). Em LAN com RTT ≤5 ms, 1-2 retransmissões cabem em ~15-30 ms → complemento ao FEC (v1.2). Não adotar a lib; só a ideia.
- **DroidCam/Iriun**: sem fonte pública verificada; abordagem conhecida: app Android + cliente Windows com driver de câmera virtual, vídeo MJPEG/H.264 sobre TCP/USB (adb forward / iproxy). **[não verificado]** — nada a copiar além do que OWC já faz.
- Qualcomm "Building Media-Rich Android Apps with MediaCodec and Vendor Extensions" (qualcomm.com/developer/blog/2023/07) e `docs.qualcomm.com/.../mediacodec_extensions.md`: confirmam que extensões vendor são aceitas no `configure()` e podem ser enumeradas (`getSupportedVendorParameters`).

## 7. Checklist de implementação (ordem sugerida)

1. Android: `createFormat()` NDK com tabela §2.1 + escada de fallback §2.2 (1 dia).
2. PC: `patchH264Sps()` + `MF_LOW_LATENCY`/`CODECAPI_AVLowLatencyMode` + loop ProcessInput/ProcessOutput imediato (1-2 dias). Medir latência antes/depois.
3. `fecLayout()` + RS GF(256) em `protocol/` com testes de round-trip (perda aleatória 0-30 %, rajadas) (2-3 dias); emissor/receptor.
4. Pacing + `sendmmsg` + buffers (§3.2).
5. Política de keyframe (§3.4) + adaptativo (§3.5), logs de `ReceiverReport` para calibrar constantes.
6. JNI de probing (`MediaCodecList`, `FEATURE_IntraRefresh`, `getSupportedVendorParameters`) → flags para o core nativo.
