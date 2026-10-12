import type { Translation } from "./registry";

export default {
  guide:
    "Arraste o arquivo do jogo para esta janela e pressione Enter (suportados: .nsp .xci .nsz .xcz)",
  legal:
    "Você deve possuir o jogo e extrair suas próprias chaves. NXbox Pack nunca baixa jogos ou chaves.",
  keys: "Arraste seu prod.keys para esta janela e pressione Enter",
  extra: "Arraste um update/DLC ou pressione Enter para continuar (um arquivo por vez)",
  host: "Endereço do Xbox (Dev Home > Acesso remoto)",
  found: "Possíveis Xbox encontrados",
  user: "Usuário do Device Portal",
  password: "Senha do Device Portal (oculta)",
  saved: "Usar credenciais do armazenamento seguro do sistema? [S/n]",
  remember: "Salvar credenciais no armazenamento seguro do sistema? [s/N]",
  secureUnavailable: "Armazenamento seguro indisponível; credenciais ficam apenas nesta sessão.",
  fallback:
    "A extração de metadados do Control NCA ainda não está implementada. Usando ID/nome do arquivo e um ícone genérico do NXbox.",
  title: "ID do jogo base (16 dígitos hexadecimais começando com 0100)",
  name: "Nome do jogo",
  runtime: "Baixando e verificando o runtime do NXbox…",
  install: "Registrando seu aplicativo independente do jogo…",
  upload: "Enviando para o LocalState privado deste aplicativo…",
  done: "Pronto. Abra o ícone deste jogo no Dev Home do Xbox; defina seu tipo como Game para usar toda a memória disponível.",
  failed: "NXbox Pack falhou",
  file: "Escolha um arquivo .nsp, .xci, .nsz ou .xcz existente e não vazio.",
  keyFile: "Escolha um arquivo existente e não vazio chamado prod.keys.",
  titleInvalid:
    "O ID do jogo base deve ter 16 dígitos hexadecimais, começar com 0100 e terminar com 000.",
  nameInvalid: "Informe um nome de 1–64 caracteres sem caracteres de controle.",
  compressed:
    "NSZ/XCZ precisa do descompressor opcional nsz no PATH (nsz -D). Instale-o ou forneça um dump NSP/XCI.",
  runtimeMissing:
    "Runtime indisponível; downloads do CI podem exigir NXBOX_PACK_GITHUB_TOKEN. Informe --runtime-url e --runtime-sha256 (ZIP do CI ou APPX de jogo).",
  runtimeInvalid: "Falha no download, checksum ou validação do runtime.",
  portal: "Falha na requisição ao Device Portal. Confira endereço, credenciais e Dev Mode.",
  deploy:
    "O registro no Xbox falhou ou excedeu o tempo. Confira o VCLibs e o log de implantação no Device Portal.",
  interrupted: "Cancelado.",
  secret: "É necessário um terminal para digitar uma senha oculta.",
  args: "Argumentos inválidos. Execute --help.",
  progress: "Enviando",
  itemGame: "Jogo",
  itemKeys: "Chaves (prod.keys)",
  itemUpdate: "Update",
  itemDlc: "DLC",
  none: "-",
  rejectNoId:
    "Rejeitado: o nome do arquivo não tem um único [ID do título], então não dá para associar ao jogo.",
  rejectMismatch:
    "Rejeitado: este ID de título não pertence ao jogo (esperado o ID do update ou a faixa de DLC).",
  rejectDuplicate: "Rejeitado: este arquivo ou ID de título já foi adicionado.",
  help: "NXbox Pack\nUso: nxbox-pack [--runtime-url URL_HTTPS --runtime-sha256 HEX] [--insecure-portal]\n--insecure-portal aceita o certificado TLS autoassinado do Xbox somente nesta conexão.\nRuntime padrão: última release de NspxMiguel/NXbox com digest SHA-256 do arquivo.\nJogos e chaves ficam fora do runtime preparado. NSZ/XCZ requer nsz no PATH.",
} satisfies Translation;
