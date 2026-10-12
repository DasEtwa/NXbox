export type Language = "pt" | "en";
export function detectLanguage(
  env: Record<string, string | undefined> = process.env,
  locale = Intl.DateTimeFormat().resolvedOptions().locale,
): Language {
  if (env.NXBOX_PACK_LANG === "pt" || env.NXBOX_PACK_LANG === "en") return env.NXBOX_PACK_LANG;
  const language = env.LC_ALL || env.LC_MESSAGES || env.LANG || locale;
  return /^pt(?:[-_.@]|$)/i.test(language) ? "pt" : "en";
}
export const strings = {
  en: {
    guide: "Drag the game file into this window and press Enter (supported: .nsp .xci .nsz .xcz)",
    legal:
      "You must own the game and dump your own keys. NXbox Pack never downloads games or keys.",
    keys: "Drag your prod.keys into this window and press Enter",
    extra: "Drag an update/DLC file, or press Enter to continue (one file at a time)",
    host: "Xbox address (Dev Home > Remote Access)",
    found: "Xbox candidates found",
    user: "Device Portal username",
    password: "Device Portal password (hidden)",
    saved: "Use credentials from the OS secure store? [Y/n]",
    remember: "Save credentials in the OS secure store? [y/N]",
    secureUnavailable: "Secure storage is unavailable; credentials are kept only for this session.",
    fallback:
      "Control NCA metadata extraction is not implemented. Using the filename title ID/name and a generic NXbox icon.",
    title: "Base game title ID (16 hexadecimal digits starting with 0100)",
    name: "Game display name",
    runtime: "Downloading and verifying the NXbox runtime…",
    install: "Registering your independent game app…",
    upload: "Uploading to this app's private LocalState…",
    done: "Ready. Open this game's tile in Xbox Dev Home; set its app type to Game for the full memory budget.",
    failed: "NXbox Pack failed",
    file: "Choose an existing, nonempty .nsp, .xci, .nsz or .xcz file.",
    keyFile: "Choose an existing, nonempty file named prod.keys.",
    titleInvalid:
      "A base game title ID must contain 16 hex digits, begin with 0100 and end with 000.",
    nameInvalid: "Enter a game name of 1–64 characters without control characters.",
    compressed:
      "NSZ/XCZ needs the optional nsz decompressor on PATH (nsz -D). Install it or provide an NSP/XCI dump.",
    runtimeMissing:
      "Runtime unavailable; CI downloads may require NXBOX_PACK_GITHUB_TOKEN. Supply --runtime-url and --runtime-sha256 (CI artifact ZIP or game APPX).",
    runtimeInvalid: "Runtime download, checksum or payload validation failed.",
    portal: "Device Portal request failed. Check the address, credentials and Dev Mode.",
    deploy:
      "Xbox registration failed or timed out. Check VCLibs and the Device Portal deployment log.",
    interrupted: "Cancelled.",
    secret: "A terminal is required to enter a hidden password.",
    args: "Invalid arguments. Run --help.",
    progress: "Sending",
    itemGame: "Game",
    itemKeys: "Keys (prod.keys)",
    itemUpdate: "Update",
    itemDlc: "DLC",
    none: "-",
    rejectNoId:
      "Rejected: no single [title ID] token in the file name, so it cannot be matched to the game.",
    rejectMismatch:
      "Rejected: this title ID does not belong to the game (expected its update ID or DLC range).",
    rejectDuplicate: "Rejected: this file or title ID was already added.",
    help: "NXbox Pack\nUsage: nxbox-pack [--runtime-url HTTPS_URL --runtime-sha256 HEX] [--insecure-portal]\n--insecure-portal accepts the Xbox self-signed TLS certificate for this connection only.\nDefault runtime: latest NspxMiguel/NXbox release with a SHA-256 asset digest.\nNo games or keys are included in the staged runtime. NSZ/XCZ requires nsz on PATH.",
  },
  pt: {
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
  },
} as const;
export type Message = keyof typeof strings.en;
export class PackError extends Error {
  constructor(
    readonly code: Message,
    options?: ErrorOptions,
  ) {
    super(code, options);
  }
}
