# OWC (Old-phone WebCam)

OWC transforma um celular Android antigo (ou atual) em uma webcam de baixa latência para o Windows.
O vídeo sai da câmera do celular, é codificado em H.264/HEVC pelo hardware do aparelho, chega ao PC
por USB ou Wi-Fi e é entregue como webcam virtual ("OWC Webcam") para Teams, Zoom, Chrome etc., além de
uma fonte nativa no OBS Studio ("OWC Phone Camera"). O PC controla a câmera do celular remotamente
(foco, exposição, zoom, resolução, bitrate e demais parâmetros).

## Recursos

- Conexão por **USB** (via adb) ou **Wi-Fi** (TCP ou UDP), com descoberta automática na rede local.
- Modo **alta velocidade** (~50 ms de captura à exibição) em 720p60 pelo hardware de câmera do aparelho.
- **Operação com a tela desligada**: o celular continua transmitindo.
- **Webcam virtual** de até **4K** (3840x2160) via filtro DirectShow próprio, compatível com Windows 10 e 11.
- **Fonte nativa no OBS** (plugin `owc-obs-source`, sem buffer extra).
- **Controle remoto completo** da câmera a partir do Windows, com presets de latência, equilíbrio e qualidade e modo manual.
- **FEC** e **bitrate adaptativo** (ABR) para redes Wi-Fi instáveis.
- **Negociação automática de codec**: H.264 por padrão, HEVC quando ambos os lados suportam.

## Latência medida

Medição do sensor ao PC num Samsung Galaxy Note 9 (valores típicos, sujeitos a variação de cena e de rede):

| Modo | Resolução / FPS | Transporte | Latência |
|---|---|---|---|
| Normal | 1080p30 | USB / Wi-Fi | ≈ 112–120 ms |
| Alta velocidade | 720p60 | USB | ≈ 48–52 ms |
| Alta velocidade | 720p60 | Wi-Fi UDP | ≈ 52 ms |
| Normal | 4K30 | USB / Wi-Fi | ≈ 195 ms |

## Requisitos

- **Celular:** Android 8.0 ou superior (API 26), com câmera Camera2 e encoder H.264 por hardware.
- **PC:** Windows 10 ou 11 x64.
- **OBS Studio 28+** (opcional, apenas para a fonte no OBS).
- **HEVC Video Extensions** da Microsoft Store (opcional, apenas se quiser HEVC).

## Início rápido

1. **Celular:** instale o `OWC.apk`. Abra o app
   OWC e conceda a permissão de câmera. Na primeira execução, aceite a isenção de otimização de bateria
   e desative a otimização para o app (ver dicas).
2. **Windows:** execute `OWC.exe`.
   - Cabo USB: clique em **"Conectar via USB"** (depuração USB ativada no celular).
   - Wi-Fi: escolha o celular na lista de dispositivos descobertos.
3. **Webcam virtual:** na aba **"Saídas"**, clique em **"Instalar"** (instala para o usuário atual, sem administrador).
   Alternativa por script, a partir da pasta do pacote:
   ```powershell
   .\install-virtualcam.ps1 -PerUser -Dll64 .\vcam\x64\owc_virtualcam.dll -Dll32 .\vcam\x86\owc_virtualcam.dll
   ```
   Para instalar para todas as contas (máquina inteira), omita `-PerUser` e aceite o UAC.
   Para remover: `.\uninstall-virtualcam.ps1 -PerUser`.
4. **OBS (opcional):** feche o OBS e rode `.\install-obs-plugin.ps1 -BuildDir .\obs-plugin`
   (requer permissão de escrita em `%ProgramData%\obs-studio\plugins`; use PowerShell como administrador se necessário).
   Depois abra o OBS e adicione a fonte **"OWC Phone Camera"**.

### Usando em Teams, Zoom e Chrome

Selecione **"OWC Webcam"** como câmera nas configurações do aplicativo. Apps que já estavam abertos
antes da instalação precisam ser reiniciados para listar a nova câmera.

### Usando no OBS

Adicione a fonte **"OWC Phone Camera"** na cena. Por ser uma fonte nativa e sem buffer, tem menor latência
do que capturar a webcam virtual.

## Dicas

- **Tela desligada:** funciona. O app usa um serviço em primeiro plano e um modo "tela preta" (brilho mínimo).
- **Menor latência:** use o modo **"Alta velocidade"** (720p60) e conexão USB, ou Wi-Fi 5 GHz em UDP.
- **Wi-Fi:** prefira a banda de 5 GHz, roteador próximo e sem outros dispositivos em streaming pesado.
- **Samsung:** em *Configurações > Bateria*, e em **"Apps que nunca entram em modo de suspensão"** inclua o OWC. Sem isso, o Android pode encerrar a transmissão.
- **Presets:** "Ultra baixa latência" é o padrão. Para mais qualidade, use "Equilibrado" ou "Qualidade máxima"
  (aumentam a latência).

## Firewall

Para a descoberta de dispositivos na rede Wi-Fi, o Windows precisa permitir **UDP 47803 de entrada** nas
redes **Privadas**. O app oferece adicionar essa regra. Portas usadas pelo app: TCP 47800 (controle, e vídeo
no modo USB), UDP 47801 (vídeo UDP) e UDP 47802 (descoberta por broadcast).

## Compilando a partir do código

### Ferramentas

- **Visual Studio 2019 Build Tools** com o workload C++ (MSVC 14.29) e **Windows SDK 10.0.19041**
  (o CMake do próprio VS2019 é usado automaticamente, ou um `cmake` no PATH).
- **Android SDK** com **NDK 27.3.13750724** e **JDK 17**.

### Build completo

```powershell
pwsh -File scripts\build-all.ps1                 # Release: Windows x64 + testes + vcam x86 + APK
pwsh -File scripts\build-all.ps1 -Config Debug   # build Debug
pwsh -File scripts\build-all.ps1 -SkipAndroid    # só Windows
pwsh -File scripts\build-all.ps1 -SkipTests      # sem ctest
```

Saídas: `build\release\out\Release\OWC.exe`, `build\release\out\Release\owc_virtualcam.dll` (x64),
`build\release-x86\out\Release\owc_virtualcam.dll` (x86), `build\release\out\obs-plugins\owc-obs-source\`
e `android\app\build\outputs\apk\debug\app-debug.apk`.

### Pacote para distribuição

```powershell
pwsh -File scripts\package.ps1
```

Gera `dist\OWC\` e `dist\OWC-<yyyyMMdd>.zip` com o executável, as DLLs x64/x86, a pasta do plugin do OBS,
o APK, os scripts de instalação, este README e `THIRD_PARTY_NOTICES.txt`.

### Build manual (Windows)

```powershell
cmake -S windows -B build/win -G "Visual Studio 16 2019" -A x64
cmake --build build/win --config Release
ctest --test-dir build/win -C Release
```

### Android

```powershell
cd android
.\gradlew.bat assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

## Estrutura do repositório

```
android/                 App Android: UI em Kotlin e núcleo nativo NDK (câmera, encoder, rede)
  app/src/main/cpp/      núcleo nativo: câmera (ACamera), encoder (AMediaCodec), transporte
protocol/                protocolo binário v1 (C++17 portátil), contrato em protocol/include/owc/protocol.h, testes
windows/
  common/                contratos compartilhados (shm.h: barramento de frames NV12)
  core/                  owc_core: conexão, depacketizer, decoder Media Foundation, ShmWriter, adb, descoberta
  app/                   OWC.exe: interface Dear ImGui + D3D11
  virtualcam/            owc_virtualcam.dll: filtro DirectShow "OWC Webcam"
  obs-plugin/            owc-obs-source.dll: fonte nativa do OBS
  tools/                 ferramentas de desenvolvimento (gerador de padrão de teste, visualizador de shm, etc.)
  third_party/           Dear ImGui (MIT) e headers do libobs (GPL-2.0-or-later)
scripts/                 build-all.ps1, package.ps1, instalação e remoção da webcam virtual e do plugin do OBS
docs/                    arquitetura e pesquisa de latência
```

## Documentação

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): componentes, fluxo de vídeo, decisões, portas e contrato JNI.
- [docs/LOW_LATENCY_RESEARCH.md](docs/LOW_LATENCY_RESEARCH.md): pesquisa sobre latência ponta a ponta.
- [docs/DEVICE_LATENCY_ADVANCED.md](docs/DEVICE_LATENCY_ADVANCED.md): análise avançada da latência no aparelho (câmera/ISP, encoder, transporte).

## Licença

Licença: a definir (o plugin do OBS é GPL-2.0-or-later por linkar a libobs).
Terceiros incluídos e seus termos estão em `THIRD_PARTY_NOTICES.txt` (no pacote).
