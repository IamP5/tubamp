import type { Dictionary } from "./en.ts";

/**
 * Brazilian Portuguese. Same voice as the English: plain, short, a little dry,
 * sentence case, no exclamation marks. Words Brazilian guitarists use in
 * English stay in English (amp, cab, gate, drive, delay, reverb, preset, knob).
 * macOS menu names are the ones macOS shows in Portuguese.
 */
export const ptBR: Dictionary = {
  dateLocale: "pt-BR",
  meta: {
    title: "tubamp. Seu timbre em um só plugin.",
    description:
      "Um Audio Unit gratuito para o Logic Pro: capturas neurais de amps reais, respostas ao impulso de cab e uma cadeia de efeitos que você põe na ordem que quiser. Apple Silicon e Intel.",
  },
  hero: {
    line: "Amp, cab e efeitos para o Logic Pro.",
    headline: ["Seu timbre em", "um só plugin."],
    what: "A captura neural de um amp real, um cab e sete efeitos. Coloque na ordem que quiser e adicione os seus plugins.",
    download: "Baixar grátis para Mac",
    tryIt: "Experimente no navegador",
  },
  release: {
    noAccount: "Sem conta",
    orLater: "{os} ou posterior",
    archs: "Apple Silicon e Intel",
  },
  nav: {
    label: "Principal",
    download: "Baixar",
    downloadMac: "Baixar para Mac",
    items: { chain: "Cadeia", captures: "Capturas", effects: "Efeitos", try: "Experimente", faq: "Perguntas" },
  },
  blocks: {
    gate: "Fecha no ruído entre as notas.",
    comp: "Nivela a palhetada.",
    drive: "Empurra a entrada do amp.",
    amp: "Uma captura neural de um amplificador real.",
    cab: "Uma resposta ao impulso de um alto-falante e um microfone reais.",
    eq: "Paramétrico ou gráfico, depois do cab.",
    mod: "Chorus, phaser, tremolo.",
    delay: "Repetições digitais, de fita ou analógicas, em estéreo.",
    reverb: "Plate, hall, room ou spring.",
  },
  facts: {
    label: "Em resumo",
    items: [
      { big: "TONE3000", small: "A biblioteca de capturas, navegável dentro do plugin. Powered by TONE3000." },
      { big: "9", small: "Blocos na cadeia: um amp, um cab e sete efeitos, em qualquer ordem." },
      { big: "A2", small: "O formato de captura NAM mais recente. A1 e os seus próprios arquivos .nam também carregam." },
      { big: "3", small: "Slots que hospedam os seus próprios efeitos Audio Unit." },
    ],
  },
  chain: {
    kicker: "Cadeia",
    headline: ["Monte a cadeia.", "Em qualquer ordem."],
    body: "O amp é um bloco entre nove. Arraste qualquer um para outro lugar, remova, adicione de novo, adicione um segundo delay. A ordem faz parte do som e é salva com o seu projeto.",
    board: {
      label: "A cadeia. Arraste um bloco, ou foque um e use as setas do teclado, para mudar a ordem.",
      waveform: "O sinal que esta ordem produz.",
      rest: "A ordem padrão.",
      tail: "O reverb agora alimenta o drive, então a cauda inteira distorce.",
      first: "{block} agora vem primeiro.",
      after: "{block} agora vem depois de {other}.",
      position: "{block}, {n} de {total}",
      earlier: "Mover {block} para antes",
      later: "Mover {block} para depois",
      reset: "Redefinir",
    },
  },
  captures: {
    kicker: "Capturas",
    headline: ["Um amp real,", "capturado."],
    body: "Uma captura é um arquivo que guarda como um amplificador real responde a uma guitarra. O tubamp toca capturas do Neural Amp Modeler, incluindo o formato A2, mais recente. Navegue pela biblioteca da TONE3000 dentro do plugin, ou carregue qualquer arquivo .nam que você já tenha.",
    facts: [
      "Capturas NAM A1, A2, slimmable e LSTM",
      "Reamostra quando a taxa da sessão é diferente da taxa da captura",
      "Nivelamento de volume opcional entre capturas",
      "Respostas ao impulso de cab, da TONE3000 ou dos seus próprios arquivos",
    ],
    attribution: "Powered by TONE3000",
  },
  effects: {
    kicker: "Efeitos",
    headline: ["Cada bloco mostra", "o que está fazendo."],
    body: "Os efeitos vêm com o plugin; não tem loja nem nada para desbloquear. Cada editor desenha o seu efeito a partir dos mesmos números que os knobs definem: o threshold do gate sobre o que você toca, a curva do compressor, as repetições do delay, a cauda do reverb. Pegue no desenho ou pegue no knob.",
  },
  more: {
    kicker: "Mais",
    headline: ["Espaço para", "o seu equipamento."],
    items: [
      {
        title: "Seus plugins, na cadeia",
        body: "Três slots hospedam os seus próprios efeitos Audio Unit em qualquer ponto da cadeia. As configurações deles são salvas com o projeto.",
      },
      {
        title: "Duas vias",
        body: "Divida o sinal, rode um segundo amp na via B e misture as duas em um mixer com volume e pan para cada via e inversão de fase.",
      },
      {
        title: "Presets e A/B",
        body: "Os presets recuperam a captura, o cab e a cadeia. Dois slots de comparação guardam duas versões de um som e alternam com um clique.",
      },
    ],
    slots: {
      label: "Uma cadeia com três dos seus próprios plugins Audio Unit colocados entre os blocos.",
      yours: "Seu plugin",
    },
  },
  demo: {
    kicker: "Experimente",
    headline: ["Este é o editor.", "Vai, pode mexer."],
    body: "A interface real, rodando no seu navegador com um substituto para o motor de áudio. Carregue um preset, arraste um bloco, gire um knob.",
    narrow: "O editor ao vivo precisa de uma tela mais larga.",
    frameTitle: "O editor do tubamp, rodando na página",
    start: "Iniciar o editor",
    use: "Usar o editor",
    live: "Pressione Esc ou clique fora do editor para rolar a página.",
    idle: "Clique no editor para usar de novo.",
    missing: "O editor ao vivo não está nesta versão da página.",
  },
  download: {
    kicker: "Saída",
    headline: ["Pluga aí."],
    body: "É instalado como Audio Unit; abra o Logic Pro e encontre em Audio Units, t0audio, tubamp.",
    free: "Grátis, sem conta.",
    unsignedNote: "O instalador ainda não é assinado com um Apple Developer ID.",
    source: "Leia o código-fonte no GitHub",
    getOnGitHub: "Baixar no GitHub",
    downloadFor: "Baixar para",
    copyHash: "Copiar o SHA-256 completo do instalador para {arch}",
    copy: "Copiar",
    copied: "Copiado",
    install: "Instalação",
    requirements: "Requisitos",
    platforms: "{os} ou posterior. Apple Silicon e Intel.",
    hosts: "Logic Pro. Outros hosts de Audio Unit não foram testados.",
    whatChanged: "O que mudou",
    questions: "Perguntas",
    gatekeeper: {
      q: "O macOS diz que não consegue verificar o instalador. Isso é esperado?",
      a: "Sim. O instalador ainda não é assinado com um Apple Developer ID, então o macOS o bloqueia na primeira vez. Abra Ajustes do Sistema, Privacidade e Segurança, e escolha Abrir Mesmo Assim.",
    },
    steps: [
      { text: "Baixe o instalador para o seu Mac.", unsigned: "" },
      {
        text: "Abra e siga as instruções.",
        unsigned:
          "Abra. O macOS o bloqueia na primeira vez: vá em Ajustes do Sistema, Privacidade e Segurança, e escolha Abrir Mesmo Assim. Depois siga as instruções.",
      },
      { text: "Reinicie o Logic Pro e adicione o tubamp a uma trilha de áudio.", unsigned: "" },
    ],
  },
  guide: {
    started: "Seu download começou",
    close: "Fechar",
  },
  share: {
    label: "Enviar o link para o meu Mac",
    short: "Enviar para o Mac",
    copied: "Link copiado",
  },
  faq: [
    {
      q: "Preciso ter minhas próprias capturas?",
      a: "Não. Entre com uma conta da TONE3000 e o plugin navega pela biblioteca da TONE3000 e carrega uma captura com um clique. Qualquer arquivo .nam que você já tenha carrega sem precisar de conta. As respostas ao impulso de cab funcionam do mesmo jeito.",
    },
    {
      q: "Preciso de outros plugins para ter um som completo?",
      a: "Não. O amp, o cab e os efeitos estão todos no tubamp, então um insert na trilha já dá o timbre completo. Os três slots de Audio Unit são para os plugins que você já tem e quer na cadeia.",
    },
    {
      q: "É grátis mesmo?",
      a: "Sim. Não tem conta, e-mail nem período de teste. O botão baixa o instalador, e o código-fonte está no GitHub.",
    },
    {
      q: "Em quais Macs ele roda?",
      a: "macOS 11 ou posterior, em Apple Silicon e Intel. Cada um tem o seu instalador, e a página escolhe o seu quando consegue identificar.",
    },
    {
      q: "Funciona fora do Logic Pro?",
      a: "O tubamp é um Audio Unit, desenvolvido e testado no Logic Pro. Outros hosts de Audio Unit não foram testados.",
    },
  ],
  footer: {
    source: "Código-fonte no GitHub",
    releases: "Todas as versões",
    language: "Idioma",
    credits: [
      "NeuralAmpModelerCore, de Steven Atkinson (MIT)",
      "JUCE 8",
      "Geist, da Vercel (OFL)",
      "Powered by TONE3000",
    ],
  },
  media: {
    "chain-build": "Blocos sendo adicionados à cadeia no board e arrastados para uma nova ordem.",
    presets: "Passando pelos presets; a cadeia se reorganiza a cada um.",
    t3k: "O navegador da TONE3000 aberto dentro do plugin, rolando as capturas e carregando uma.",
    amp: "O editor do amp passando pelas capturas da biblioteca.",
    split: "Uma cadeia dividida em duas vias, com um segundo amp e um mixer.",
    compare: "Alternando entre os slots de comparação A e B.",
    "fx-gate": "O editor do gate: o nível de entrada ao longo do tempo, com um threshold que dá para arrastar.",
    "fx-comp": "O editor do compressor: a curva de transferência mudando com o threshold e o ratio.",
    "fx-drive": "O editor do drive: a curva do waveshaper se dobrando conforme o ganho sobe.",
    "fx-cab": "O editor do cab: cortes de graves e de agudos na curva de resposta.",
    "fx-eq": "O editor do EQ: bandas sendo arrastadas na curva de resposta.",
    "fx-mod": "O editor de modulação: a onda do LFO mudando com a velocidade e a profundidade.",
    "fx-delay": "O editor do delay: as repetições se espalhando conforme o tempo e o feedback mudam.",
    "fx-reverb": "O editor do reverb: a cauda crescendo com o decay.",
    tubes: "Filamentos de válvulas brilhando em âmbar no escuro.",
    strings: "Três cordas de guitarra encapadas iluminadas em âmbar sobre preto, uma delas vibrando.",
    "capture-720": "Um cabeçote valvulado se desfazendo em partículas âmbar que se assentam em uma única linha de forma de onda.",
    speaker: "Um cone de alto-falante preto com um contorno de luz âmbar, com poeira se levantando dele.",
  },
};
