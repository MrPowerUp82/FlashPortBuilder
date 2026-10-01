Estou começando o som pelo lado do build (decodificar tudo para PCM) e depois o mixer no runtime. Lendo os stubs atuais:

Agora o CMake (mpg123 opcional) e a seção de som no pack:

Nada aplicado. Vou usar Write para o trecho C++ (sem heredoc) e um script Python separado:

Integrando o mixer ao runtime (structs, carga do pack, ganchos de quadro e de botão):

Agora o objeto `Sound` do AVM1 (substitui o stub silencioso):

O `\n` quebrou outra vez; corrigindo:

Caminho `/tmp` do Git Bash vs Windows nativo. Usando o scratchpad:

Escrevendo as classes de som do AS3 (Sound, SoundChannel, SoundTransform, SoundMixer):

Aqui o `\n` já está correto no arquivo (meu match tinha escape duplo). Usando Edit:

Os seis jogos agora têm som. Eu não consigo ouvir o áudio, então confirmei por medição: gravei o que o mixer produz e olhei nível e quando cada trecho começa. Nenhum dos seis passou por teste de escuta humana.

O que toca:
- **Sons de evento e de timeline:** os `StartSound`, com repetições, trechos de início e fim, envelopes de volume e as flags de parar e não sobrepor.
- **Sons de stream:** começam com a timeline e param quando ela é destruída.
- **Sons de botão:** rollover, rollout, clique e soltar.
- **ActionScript 2:** o objeto `Sound` com `attachSound`, `start` (com offset e repetições), `stop`, volume, pan, `duration` e `position`. Também `onSoundComplete` e `stopAllSounds`. Os controles de volume e pan por script funcionam.
- **ActionScript 3:** `Sound`, `SoundChannel`, `SoundTransform` e `SoundMixer`, incluindo as classes de som embutidas e o evento de fim de som.

Como funciona:
- **Build:** o builder decodifica PCM, ADPCM e MP3 e grava o resultado no `movie.pack`. O MP3 depende do `libmpg123`, que instalei no MSYS2 e já está no CMake como opcional. Sem ele, os MP3 ficam mudos.
- **Runtime:** o runtime mistura tudo em 44,1 kHz estéreo pelo SDL e tem um limitador suave para os efeitos sobrepostos não distorcerem.
- **Formato:** o pack passou para a versão 6, então os packs antigos precisam ser regerados.

O que mediu:

| Jogo | Resultado |
|---|---|
| Mike Shadow | silêncio até a tela de menu, depois música e efeitos |
| Comic Stars | música desde o título |
| Dragon Ball | música em loop e golpes, bem alto: com vários golpes sobrepostos ainda satura bastante, porque os sons do jogo já são fortes |
| Mortal Kombat Karnage | silêncio nos menus, som só na luta |
| Superman | áudio contínuo |
| Toon Cup | áudio contínuo |

Isso bate com a minha leitura dos jogos, mas pode ser que algum menu devesse ter som e não tem.

Também verifiquei que o dispositivo de áudio abre numa janela normal e que o exe funciona sem o MSYS2 no PATH. Rodei de novo as mesmas sequências de jogo de antes. As telas ficaram iguais, exceto o Comic Stars, cujos rótulos "Music/Menu" já apareciam desde a correção anterior.

Ficam mudos: streams em Nellymoser e Speex, e sons carregados da rede. Nenhum dos seis jogos usa esses formatos nos trechos que testei.

Para depurar: `FP_TRACE_AUDIO=1` mostra cada som que começa, com nível e taxa, e `--audio-out arquivo.wav` junto com `--capture` salva o áudio misturado. Atualizei o `README.md` e o grafo do graphify. Não fiz commit.

---

Implementando o lado do builder (bundle de diretório):

Implementando domínios de aplicação no AVM2:

Ajustando o ctor de Sound para usar `characterForClass`:

Mesmo bug do `\n` em heredoc; corrigindo a linha 1015:

Agora a classe `BitmapData` real (draw, getColorBoundsRect, pixels):

Implementando blend modes de ponta a ponta:

Mesmo problema (o Python converte duas vezes). Usando Edit direto:

Cheguei ao limite de uso antes de terminar. O Ben 10 Sumo Slammer Samurai está jogável de verdade, o Omniverse chega à seleção de personagem, e o Generator Rex só foi verificado até a tela de título.

**Resultado por jogo**
- **Ben 10 Sumo Slammer Samurai:**
  - Funciona do menu até a fase: instruções, fase carregada de outro arquivo, Ben correndo e pulando, caixas bloqueando, moedas e HUD.
  - Os textos do menu são um arquivo que escrevi à mão, porque o `texts-eng.xml` original não veio no download.
- **Ben 10 Omniverse:**
  - Chega a título, menu, escolha de 1 ou 2 jogadores e seleção de personagem.
  - O clique em Ben ou Rook não responde, então ainda não entrei na fase.
  - Também usa texto escrito por mim (só uma entrada), então vários rótulos saem em branco.
- **Generator Rex Heroes United:** carrega todos os arquivos e mostra a tela de título. Não passei dali, e o texto também é provisório.

**Como funciona agora**
- `flashport build <pasta>` empacota o `launcher.swf` como jogo principal e converte cada outro SWF em `data/*.pack`, junto com os arquivos XML.
- O runtime lê esses arquivos em tempo de execução, mescla os personagens de cada um (cada SWF carregado vira um "domínio" separado) e entrega o evento de conclusão.
- Para isso precisei implementar `XML`/`XMLList` (E4X), `BitmapData` real (`draw`, `getColorBoundsRect`), `Vector`, `Vector3D`, blend modes e repetição de texturas. Também adicionei domínios de aplicação, `URLLoader` lendo arquivos e interfaces do AS3.
- Mudanças que valem para os jogos antigos também:
  - Fills com alpha 0 agora contam como área de clique, para botões com hit area invisível.
  - Corrigi um erro de `gotoAndStop` com label inexistente.
- Ferramentas de depuração novas: `FP_TRACE_STACK`, `FP_TRACE_EVENT`, `FP_TRACE_HIT`, `FP_DUMP_BOUNDS`.

**O que falta**
1. **Cliques na seleção do Omniverse.** Os botões "benRex/rexBen" têm área de clique grande, mas as sombras desenhadas por cima (formas soltas na linha do tempo) interceptam o clique. Eu estava investigando se, no Flash real, essas formas deixam o clique passar. Cheguei a conferir que não usam `cacheAsBitmap`, blend nem filtros, mas não concluí.
2. **Fases do Omniverse e do Rex.** Ainda não testei nenhuma fase, e provavelmente haverá mais faltas.
3. **Regerar os jogos antigos.** O pack subiu para a versão 7, então os packs dos seis jogos anteriores precisam ser reconstruídos. Depois disso, é preciso refazer as comparações de captura para ver que nada quebrou. Não fiz isso nesta rodada.
4. **Conferir a repetição de texturas** (a quebra de triângulos no builder) com um SWF que realmente use fundo em repetição. Não consegui validar com o título do Omniverse, porque aquele "glitch" era uma animação do próprio jogo.
5. **Textos originais.** Se você tiver os `texts/*.xml` reais, basta colocar em `games/multi_files/<jogo>/texts/`.
6. **README.** Ainda não documenta nenhuma dessas novidades, e não fiz commit.

Para continuar, rode `build/flashport.exe build games/multi_files/<pasta> --output output/<nome>`, depois compile em `output/<nome>/build` e rode `flash_game.exe`.