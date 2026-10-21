#include "params_meta.h"

#include <algorithm>

namespace owcapp {

using owc::ParamId;

static const PMeta kMeta[] = {
    // ---------------------------------------------------------------- stream / encoder
    {ParamId::CameraIndex, "Câmera", "Escolhe qual câmera do aparelho é usada. Reinicia o stream.", PFmt::Plain},
    {ParamId::VideoWidth, "Largura", "Largura do vídeo em pixels. Resolução maior = mais detalhe, mais bitrate e mais latência de codificação.", PFmt::Plain},
    {ParamId::VideoHeight, "Altura", "Altura do vídeo em pixels.", PFmt::Plain},
    {ParamId::Fps, "Quadros por segundo", "Taxa de quadros. Mais fps = menor latência por quadro e movimento mais fluido, mas exige mais luz (exposição curta) e mais bitrate.", PFmt::Plain},
    {ParamId::VideoCodec, "Codec",
     "H.264 funciona em qualquer PC. HEVC (H.265) gera ~30-40% menos bits para a mesma qualidade, mas exige o decodificador HEVC no Windows e costuma ter um pouco mais de latência de codificação.",
     PFmt::Plain},
    {ParamId::Bitrate, "Bitrate",
     "Taxa de bits do vídeo. Mais bitrate = imagem mais limpa em movimento, porém mais carga na rede (e mais atraso se o Wi-Fi não aguentar). Com bitrate adaptativo, este valor é o teto.",
     PFmt::Mbps},
    {ParamId::BitrateMode, "Modo de bitrate",
     "CBR: taxa constante, previsível, melhor para baixa latência. VBR: taxa variável, melhor qualidade por bit mas com picos que podem causar atraso. CQ: qualidade constante, picos grandes.",
     PFmt::Plain},
    {ParamId::EncoderProfile, "Perfil do encoder",
     "Baseline: mais simples, menor latência. Main/High: compressão mais eficiente (mais qualidade por bit), habilita B-frames, com um pouco mais de atraso de codificação.",
     PFmt::Plain},
    {ParamId::BFrames, "B-frames",
     "Quadros bidirecionais melhoram a compressão, mas cada B-frame adiciona ~1 quadro de latência (33 ms a 30 fps). 0 = latência mínima. Exige perfil Main/High.",
     PFmt::Plain},
    {ParamId::KeyframeIntervalMs, "Intervalo de keyframe",
     "Intervalo entre quadros-chave (IDR). Menor = recuperação mais rápida de perdas, porém mais picos de bitrate. 0 = só sob demanda (menor bitrate, recuperação depende do pedido do PC).",
     PFmt::Ms},
    {ParamId::QpMin, "QP mínimo",
     "Limite inferior do parâmetro de quantização. Valores altos limitam a qualidade máxima (arquivos menores). 0 = automático.", PFmt::Plain},
    {ParamId::QpMax, "QP máximo",
     "Limite superior do QP. Valores baixos garantem qualidade mínima, mas podem estourar o bitrate em cenas difíceis (mais atraso). 0 = automático.", PFmt::Plain},
    {ParamId::IntraRefreshFrames, "Intra-refresh",
     "Atualiza a imagem aos poucos em vez de keyframes grandes: recuperação suave após perdas e bitrate mais estável (menos picos). Número de quadros do ciclo; 0 = desligado.",
     PFmt::Frames},
    {ParamId::MaxSendQueueFrames, "Fila de envio máxima",
     "Quantos quadros podem esperar para envio no celular. Fila pequena = latência mínima (descarta quando a rede engasga). Fila grande = mais suave em Wi-Fi ruim, porém com mais atraso.",
     PFmt::Frames},
    {ParamId::LatencyDropPolicy, "Política ao encher a fila",
     "Descartar até o keyframe: prioriza latência (a imagem pode travar/pular). Bloquear/enfileirar: prioriza qualidade (nenhum quadro perdido, mas o atraso cresce).", PFmt::Plain},
    {ParamId::AdaptiveBitrate, "Bitrate adaptativo",
     "O celular reduz o bitrate automaticamente quando a rede/fila indica congestionamento e volta a subir depois. Mantém a latência baixa em Wi-Fi instável (o bitrate configurado é o teto).",
     PFmt::Plain},
    {ParamId::FecPercent, "FEC (redundância UDP)",
     "Percentual de pacotes de paridade (Reed-Solomon) enviados no modo UDP para reconstruir pacotes perdidos sem retransmitir. Mais FEC = mais robusto a perdas, mas usa mais banda. Auto (-1) ajusta sozinho. Só vale em UDP.",
     PFmt::Percent},
    {ParamId::HighSpeedMode, "Alta velocidade (experimental, menor latência)",
     "Sessão de câmera de alta velocidade (ex.: 120 fps): menor latência do ISP, porém com controles manuais limitados e outra lista de resoluções/fps. Reinicia o stream.",
     PFmt::Plain},
    // ---------------------------------------------------------------- camera
    {ParamId::AfMode, "Modo de foco",
     "Manual desliga o autofoco (use a distância de foco). Contínuo-vídeo é o mais suave para webcam; contínuo-foto reage mais rápido.", PFmt::Plain},
    {ParamId::FocusDistance, "Distância de foco",
     "Foco manual em dioptrias (1/metros). 0 = infinito (∞). Valores maiores focam mais perto (ex.: 5 D = 20 cm). Só vale com foco manual.", PFmt::Diopter},
    {ParamId::AeMode, "Exposição automática",
     "Automática ajusta ISO e tempo de exposição sozinha. Desligada habilita ISO e tempo manuais.", PFmt::Plain},
    {ParamId::AeLock, "Travar exposição", "Congela a exposição atual (evita 'pumping' de brilho).", PFmt::Plain},
    {ParamId::ExposureCompensation, "Compensação de exposição",
     "Clareia ou escurece a imagem em passos (EV) com a exposição automática ligada.", PFmt::Signed},
    {ParamId::Iso, "ISO", "Sensibilidade do sensor (exposição manual). ISO alto = mais brilho e mais ruído.", PFmt::Plain},
    {ParamId::ExposureTimeNs, "Tempo de exposição",
     "Tempo de exposição manual. Curto = menos borrão de movimento e quadros mais rápidos; longo = mais luz, mas limita o fps.", PFmt::MsFromNs},
    {ParamId::AwbMode, "Balanço de branco", "Modo de balanço de branco (cor da iluminação).", PFmt::Plain},
    {ParamId::AwbLock, "Travar balanço de branco", "Congela o balanço de branco atual.", PFmt::Plain},
    {ParamId::Zoom, "Zoom", "Zoom digital/óptico (recorte). 1,0× = sem zoom.", PFmt::ZoomX},
    {ParamId::Torch, "Lanterna", "Liga o flash LED da câmera como luz contínua.", PFmt::Plain},
    {ParamId::AntiBanding, "Anti-cintilação", "Evita faixas de luz de lâmpadas: 50 Hz ou 60 Hz conforme a rede elétrica local.", PFmt::Plain},
    {ParamId::VideoStabilization, "Estabilização eletrônica (EIS)",
     "Estabilização digital da imagem. Pode adicionar latência e recortar o campo de visão.", PFmt::Plain},
    {ParamId::OpticalStabilization, "Estabilização óptica (OIS)", "Estabilização por movimento da lente, sem custo de latência.", PFmt::Plain},
    {ParamId::NoiseReduction, "Redução de ruído", "Nível de redução de ruído do ISP. Níveis altos suavizam detalhes e podem aumentar a latência do ISP.", PFmt::Plain},
    {ParamId::EdgeMode, "Nitidez (bordas)", "Realce de bordas do ISP. Mais realce = imagem mais nítida, com possível halo.", PFmt::Plain},
    // ---------------------------------------------------------------- device
    {ParamId::ScreenMode, "Tela preta no celular",
     "Reduz o brilho da tela ao mínimo (em OLED, praticamente desligada) enquanto a câmera continua transmitindo. Economiza bateria e calor.", PFmt::Plain},
};

const PMeta* findMeta(ParamId id) {
  for (const auto& m : kMeta)
    if (m.id == id) return &m;
  return nullptr;
}

std::string paramLabel(ParamId id) {
  if (const PMeta* m = findMeta(id)) return m->label;
  return "Parâmetro #" + std::to_string(static_cast<unsigned>(id)) + " (" + owc::toString(id) + ")";
}

const std::vector<ParamId>& cameraGroup() {
  static const std::vector<ParamId> v = {
      ParamId::AfMode, ParamId::FocusDistance, ParamId::AeMode, ParamId::AeLock, ParamId::ExposureCompensation,
      ParamId::Iso, ParamId::ExposureTimeNs, ParamId::AwbMode, ParamId::AwbLock, ParamId::Zoom, ParamId::Torch,
      ParamId::AntiBanding, ParamId::VideoStabilization, ParamId::OpticalStabilization, ParamId::NoiseReduction,
      ParamId::EdgeMode};
  return v;
}

const std::vector<ParamId>& encoderGroup() {
  static const std::vector<ParamId> v = {
      ParamId::VideoCodec, ParamId::Bitrate, ParamId::BitrateMode, ParamId::EncoderProfile, ParamId::BFrames,
      ParamId::KeyframeIntervalMs, ParamId::QpMin, ParamId::QpMax, ParamId::IntraRefreshFrames,
      ParamId::MaxSendQueueFrames, ParamId::LatencyDropPolicy, ParamId::AdaptiveBitrate, ParamId::FecPercent,
      ParamId::HighSpeedMode};
  return v;
}

bool isSpecialParam(ParamId id) {
  switch (id) {
    case ParamId::CameraIndex:
    case ParamId::VideoWidth:
    case ParamId::VideoHeight:
    case ParamId::Fps:
    case ParamId::ScreenMode:
      return true;
    default:
      break;
  }
  const auto& c = cameraGroup();
  const auto& e = encoderGroup();
  return std::find(c.begin(), c.end(), id) != c.end() || std::find(e.begin(), e.end(), id) != e.end();
}

}  // namespace owcapp
