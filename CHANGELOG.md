# Changelog

## Não lançado

### Novidades
- Novas opções em **Configurações > Gráficos**, dentro do próprio jogo:
  - **Qualidade gráfica:** de Ultra baixa a Ultra, ou Console (o visual original).
  - **Limite de quadros:** de 30 a 240 FPS, ou sem limite.
  - **V-Sync:** ligado ou desligado.
  - **Ajustes detalhados:** resolução interna (50% a 200%), sombras, detalhe do cenário, detalhe dos veículos, tesselação, névoa, efeitos de luz e detritos. Em "Auto", cada um segue a qualidade gráfica escolhida. Alguns ajustes só valem a partir da próxima corrida.
- As escolhas ficam salvas para a próxima vez, e a opção V-Sync do launcher passou a funcionar.
- Duas câmeras novas na corrida: **capô** e **cockpit** (primeira pessoa). O botão de trocar câmera (RB, ou E no teclado) agora alterna entre distante, próxima, capô e cockpit, e a última escolhida fica salva.
- Nova página **Configurações > Vozes e texto**: o idioma dos textos muda na hora, sem reiniciar. O idioma das vozes também é escolhido ali e vale a partir da próxima vez que o jogo abrir (um aviso do próprio jogo lembra disso ao trocar).
- Nova opção **Upscaler** em Configurações > Gráficos: **TAA** (o original do jogo) ou **FSR 2** (AMD), que deixa a imagem mais nítida quando a resolução interna está abaixo de 100%. Funciona em qualquer placa de vídeo e troca na hora.
- Na corrida, o **mouse gira a câmera** (como o analógico direito); parado por um instante, a câmera volta para trás do carro. Dá para desligar ou ajustar a sensibilidade no `onrush.ini` ([Controls] MouseLook / MouseSensitivity).
- O jogo abre bem mais rápido: a tela inicial não espera mais pelos servidores online, que não existem mais, e entra direto no modo offline.
- O Discord agora mostra "RushLauncher" e a versão em uso.

### Correções
- Alt+Enter agora alterna corretamente entre tela cheia e janela.

## v0.1.5 — correção do crash no Windows 10

**Como atualizar:** não é preciso apagar a pasta do launcher. A partir da v0.1.1, o próprio launcher oferece a atualização. Também dá para descompactar este zip por cima da pasta atual.

### Port
- O jogo fechava logo no primeiro quadro em versões do Windows diferentes da usada no desenvolvimento (por exemplo, Windows 10 22H2), com crash em `dxgi.dll`. Um diagnóstico antigo do bridge sobrescrevia 14 bytes do `dxgi.dll` num endereço fixo, válido só para um build específico do Windows. Esse hook foi removido.

### Tradutor
- Os pacotes do jogo também trazem ~5600 contêineres de shader sem programa do Xbox, que o jogo nunca usa. O log os mostrava como erro do Python (`AttributeError: 'NoneType' object has no attribute 'find'`), o que parecia uma falha da preparação. Agora eles aparecem como "no Xbox program (not used by the game)".

## v0.1.4 — pasta do jogo ligada a uma cópia movida ou renomeada

**Como atualizar:** não é preciso apagar a pasta do launcher. A partir da v0.1.1, o próprio launcher oferece a atualização. Também dá para descompactar este zip por cima da pasta atual.

### Launcher
- Se a cópia do ONRUSH usada na preparação for movida, renomeada ou apagada, os atalhos `dat` e `EmbeddedXvd` da pasta do jogo ficavam apontando para o nada e o jogo fechava logo ao abrir. Agora o JOGAR percebe isso, mostra onde a cópia estava, pede a pasta nova (aplicando o patch se ela não estiver na 1.80) e liga a pasta do jogo a ela de novo. O diagnóstico do log passou a conferir os pacotes de verdade (`dat\index\dat.ndx`), não só se o atalho existe.

## v0.1.3 — correções na atualização do jogo para a 1.80

**Como atualizar:** não é preciso apagar a pasta do launcher. Na v0.1.1 ou v0.1.2, o próprio launcher oferece a atualização. Também dá para descompactar este zip por cima da pasta atual. Apagar a pasta faria perder o jogo já preparado, os shaders e as configurações.

### Launcher
- Preparar o jogo de novo depois do patch falhava com "copiar SplashScreen.png (erro 32)": o launcher deixava a imagem de fundo aberta enquanto ela aparecia na tela. Agora a imagem é lida para a memória e o arquivo fica livre.
- O `onrush-pc.exe` é recriado sempre que o `game_release_xbox.exe` for mais novo que ele. Antes, se a preparação parasse no meio depois do patch, o `onrush-pc.exe` continuava sendo o jogo antigo e fechava ao abrir.

## v0.1.2 — hotfix da v0.1.1

**Como atualizar:** não é preciso apagar a pasta do launcher. Na v0.1.1 o próprio launcher oferece a atualização. Também dá para descompactar este zip por cima da pasta atual. Apagar a pasta faria perder o jogo já preparado, os shaders e as configurações.

### Launcher
- O JOGAR agora confere a versão do jogo antes de abrir. Uma pasta preparada com uma cópia 1.0.64980.34 (por exemplo, a de uma instalação da v0.1.0 reaproveitada pela v0.1.1) fazia o jogo fechar logo no início. Agora o launcher acha a cópia original pelo atalho `dat`, pede a pasta do patch, atualiza a cópia para a 1.80.100.100 e prepara a pasta do jogo de novo. Se a cópia original não for encontrada, ele pede para escolher a cópia ([#2](https://github.com/defaultdotcfg/rushlauncher/issues/2)).

## v0.1.1 — versão final do jogo, perfis, fotos e correções gráficas

**Atenção:** a partir desta versão o port mira só a versão final do jogo, **1.80.100.100**. Se a sua cópia for a 1.0.64980.34, o launcher pede a pasta do patch de atualização e aplica as diferenças sozinho (os arquivos do patch são movidos para a pasta do jogo, não copiados).

### Launcher
- O projeto agora se chama **rushlauncher**: janela e mensagens com o nome novo, verificação de atualização no repositório `defaultdotcfg/rushlauncher`.
- Novo ícone no launcher e no `onrush-pc.exe` (o `onrush-pc.exe` de instalações antigas é recriado com o ícone no próximo JOGAR).
- Logo maior e centralizado no painel lateral, em todas as abas (Playlist e Fotos mostravam só o texto ou nada).
- **Aba Perfis**: criar, renomear, excluir (vai para a Lixeira) e escolher o perfil salvo. O nome do perfil é o nome do jogador. Dois interruptores por perfil, só em tempo de execução (o save não é alterado):
  - **Desbloquear cosméticos**: libera trajes, manobras, lápides, comemorações, veículos e crashtags do modo online.
  - **Desbloquear tudo (cosméticos e eventos)**: libera também os eventos da carreira.
- **Aba Controles**: troca das teclas que fazem o papel do controle de Xbox no teclado. São duas teclas por ação (acelerar, frear, direção, direcional, botões A/B/X/Y, LB/RB, View, Menu, L3/R3 e analógico direito), com opção de limpar e de voltar ao padrão. O mapeamento fica salvo no `onrush.ini` e vale no próximo JOGAR. Teclas opostas apertadas juntas (esquerda e direita, por exemplo) se anulam, em vez de valer a última.
- O direcional do teclado agora tem teclas próprias (1, 2, 3 e 4). Antes, W, S, A, D e as setas também apertavam o direcional, e acelerar com W abria a lista de provocações na corrida.
- **Tema claro e escuro**: escolha em Configurações → Tema do launcher. A troca é na hora e vale também para a janela de preparo dos shaders.
- **Discord Rich Presence**: o launcher tentava conectar só no instante em que o jogo abria. Agora tenta de novo a cada 15 s enquanto o jogo roda, então funciona com o Discord aberto depois do jogo ou reiniciado no meio. Se o Discord recusar a conexão ou a atividade, o motivo fica no log do launcher.
- **Minimizar o launcher ao jogar** (Configurações): o launcher minimiza quando o jogo abre e volta quando o jogo fecha.
- **Aba Fotos**: galeria das fotos tiradas no jogo (F12), com marcação de várias fotos, Marcar/Desmarcar todas, abrir foto, excluir marcadas (Lixeira) e abrir a pasta.
- A aba Jogar mostra o perfil em uso e as horas de jogo dele, e traz a escolha da pasta do jogo e do patch (a aba Instalação saiu).
- A opção de ir direto ao gameplay saiu.
- Novo visual: o logo **Rush Launcher** substitui o nome na lateral e vira o ícone da janela e do executável.
- A aba Playlist está escondida por enquanto, até a função ser melhor integrada ao jogo.
- **Preparação de shaders em uma etapa, sem abrir o jogo**: o launcher lê os shaders direto dos pacotes do jogo (`dat\`) em vez de abrir o jogo várias vezes para coletá-los. Tradução e compilação acontecem num único passe paralelo, com uma barra de progresso só. Todos os shaders passam a ser compilados com o DXC (o fxc levava até 1 minuto em alguns shaders de computação), então a etapa de "otimizar quando o jogo fecha" deixou de existir.
- Tradutor mais rápido, com o mesmo resultado byte a byte: o DXC roda dentro do processo (antes abria um `dxc.exe` por shader), até 16 processos em paralelo e leitura/padrões em cache no tradutor. Os ~13 mil shaders da versão final passam de 256 s para cerca de 3 minutos (16 threads).

### Jogo
- Porte para a versão 1.80.100.100.
- Corrigido o fechamento do jogo durante o vídeo de introdução quando "pular introdução" está desligado.
- As opções de inicialização do jogo agora chegam a ele (a ponte repassava os argumentos vazios).
- Apertar Alt ou F10 sozinho não congela mais o jogo (abria o menu da janela, que travava a imagem até apertar Esc).

### Gráficos
- Personagens e pilotos voltam a aparecer (cabine, menus, garagem).
- Rosto com a pele "cristal" e árvores como cartões cinza: causa comum no tradutor (uma leitura de constantes lia o endereço já sobrescrito). Tradutor revisão 4: ao subir de versão, o launcher retraduz os shaders sozinho.
- Árvores pretas corrigidas (profundidade dos passes de árvore).
- Telas "Escolha um veículo" e "Jogo pausado" voltam a mostrar o jogo desfocado ao fundo (antes ficavam pretas). Tradutor revisão 6.
- O reflexo de lente (lens flare) do sol não atravessa mais paredes e terreno. Tradutor revisão 5.
- **Sombras do sol**: o passe da máscara de sombra em cascata era traduzido com desvios por lane em vez de por onda de 64 lanes; o terreno distante saía todo em sombra e o chão do menu pré-corrida ficava branco. Agora o mapa inteiro recebe o sol e as sombras de árvores e do carro aparecem. O CS de iluminação principal da corrida usa o mesmo tratamento (cores do ambiente e da areia bem mais próximas do Xbox). Tradutor revisão 9.
- **Névoa e atmosfera**: o passe de névoa volumétrica (froxels) também usava desvios por lane. Com a correção a névoa/bruma volta: o menu pré-corrida ganha montanhas enevoadas, chão azul-claro e céu claro, e a corrida fica com cores de areia e mar bem mais naturais.
- **Partículas da GPU ligadas**: os 22 kernels emissores que ficavam desligados voltam a rodar (a guarda dos despachos indiretos evita o travamento antigo). Efeitos como brilho/aura elétrica azul, destroços e respingos passam a existir. Tradutor revisão 8: o launcher esvazia o `skip.txt` antigo sozinho.
- **Aura do Rush na cor da equipe**: a aura e o respawn dos carros saíam laranja para todo mundo. O tradutor juntava numa só as declarações `t35` (SRV) e `u35` (UAV); a tabela de competidores (registros de 672 bytes) virava um buffer cru lido com passo de 4 a 12 bytes, e os emissores liam equipe, classe do veículo e posição do lugar errado. Agora a aura sai azul na equipe azul e com as cores próprias de cada classe. Tradutor revisão 11.
- **Rastros e franjas ciano**: carros em movimento, poeira e bordas de árvores deixavam rastros ciano/vermelhos. O upscaler temporal lia um valor de vermelho errado da memória compartilhada (uma leitura `ds_read2_b32` cujo registro de endereço é também o primeiro destino usava, na segunda metade, o endereço já sobrescrito). Tradutor revisão 12.
- **Árvores de papelão**: painéis escuros e translúcidos em volta das árvores (os cartões inteiros da folhagem). O passe de G-buffer das árvores não tem teste de alfa e depende de profundidade EQUAL contra o pré-passe para recortar as folhas; um contorno antigo do bridge (da época das árvores pretas) trocava EQUAL por GREATER_EQUAL e deixava o cartão inteiro ser desenhado. Com as posições do pré-passe e do G-buffer agora idênticas, o EQUAL original volta a ser usado.
- **Laços "waterfall" fiéis ao GCN**: o `scc` das operações de máscara de 64 bits e as saídas de laço por `vcc`/`exec` eram calculados por lane. Nos laços que escolhem a textura bindless de cada partícula, cada lane saía assim que a sua textura era lida, e as amostras seguintes ficavam sem os vizinhos do quad (derivadas e mip indefinidos). Agora a wave inteira continua no laço enquanto alguma lane estiver ativa, como no Xbox. Tradutor revisão 13.
- **Respingos de água e poeira apagados**: ao passar pelo rio, o spray quase não aparecia (translúcido, escuro e azulado), e a poeira levantada pelos carros na areia sumia. O compute shader que ilumina as partículas lia as matrizes das cascatas de sombra do sol nos registradores errados: leituras indexadas de constant buffer com 1 a 3 componentes (`tbuffer_load_format_x/xy/xyz idxen`) usavam o tamanho do formato (4/8/12 bytes) como passo do índice, mas os cbuffers são registradores float4, com passo de 16 bytes. Toda partícula dentro das cascatas ficava 100% na sombra. Agora o spray sai branco e opaco como no Xbox, e a poeira e os respingos dos outros carros voltam a aparecer. A mesma leitura existia em cerca de 1.200 outros shaders. Tradutor revisão 14.
- Shaders de captura do céu com desvios estruturados (tradutor revisão 7). O launcher retraduz os shaders sozinho ao subir de revisão.
- 12 dos 13 shaders que o tradutor recusava agora são traduzidos; os desenhos de shaders ainda não traduzidos são ignorados em vez de aparecerem como lixo.

## v0.1.0 — primeira versão pública

Primeira versão do port nativo do **ONRUSH (Xbox One 1.0.64980.34)** para Windows x64 com Direct3D 12. Nenhum arquivo do jogo vem junto: é preciso ter a sua própria cópia do pacote (veja o README).

### O que funciona
- O jogo abre e roda nativamente no PC: tela de título, EULA, menus, tutorial e corridas (testado no Countdown em Big Dune Beach), com controle XInput.
- Tradução em tempo real dos shaders do console (GCN → HLSL), gerados na sua máquina a partir da sua cópia do jogo.
- Cache de pipelines em disco: depois da primeira vez, as pistas carregam sem recompilar shaders.

### Gráficos
- Iluminação completa do G-buffer: sol, normais e relevo das superfícies, pintura dos carros.
- Luz ambiente e reflexos (cubemaps do ambiente e de irradiância).
- Céu e atmosfera pré-calculados: céu com gradiente, areia clara, água azul e sombras azuladas em vez de pretas.
- Árvores e terreno distantes voltam a aparecer (bypass da tesselação), sem buracos no horizonte ao pular rampas.
- Interface: fundo desfocado da EULA, degradês, barra de seleção, efeito VHS na câmera de batida.
- Abertura "PREPARAR / COUNTDOWN" com o carro renderizando.
- Corrigidas faixas pretas no chão, a grade de blocos coloridos em movimento e o "fantasma" verde nas rochas.

### Áudio
- Som do jogo (efeitos, música e vozes) pelo WASAPI.
- **Playlist própria**: toque suas músicas durante o jogo (a música original fica muda), com ordem aleatória e volume.

### Launcher
- **Prepara o jogo sozinho**: ao apertar JOGAR (ou ao escolher a pasta do jogo), basta apontar a sua cópia, ou seja, o pacote extraído com `Mount` e `EmbeddedXvd`. O launcher:
  - copia o executável e os arquivos pequenos (~60 MB) para a pasta dele;
  - liga `dat` e `EmbeddedXvd` como atalhos para a sua cópia;
  - cria o `onrush-pc.exe` (o jogo aparece como ONRUSH™ no Gerenciador de Tarefas).
  A sua cópia não é alterada. Também pela linha de comando: `-setup "<pasta da cópia>"`.
- **Shaders traduzidos sozinhos**, sem Python nem Windows SDK: o tradutor vem junto (`translator\`).
  - Na primeira vez, o JOGAR abre o jogo algumas vezes, em uma janela pequena e sem som, para ele gravar os shaders que usa; o launcher traduz tudo e só então abre o jogo de verdade.
  - Os shaders novos (pistas e menus que você ainda não tinha visitado) são traduzidos quando o jogo fecha, ou no próximo JOGAR.
  - Cerca de 120 shaders de computação levam de 3 s a 1 min cada para compilar. Eles são compilados primeiro no modo rápido, e a primeira preparação cai de mais de 10 minutos para poucos minutos. A versão otimizada é feita quando o jogo fecha, e dá para pular: continua na vez seguinte.
  - Usa no máximo 8 processos de compilação, limitado pela memória livre.
- Resolução, modo de tela (sem bordas, janela ou tela cheia) e VSync.
- Launcher em português, inglês ou espanhol; idioma do texto e da voz do jogo escolhidos separadamente.
- Opção de pular a introdução (tela legal e logos): título em ~2 s.
- Nome do jogador com save próprio e pasta de saves configurável.
- Modo foto: **F12** salva a imagem sem HUD em PNG ou JPG.
- Discord Rich Presence.
- Aba **Playlist**: criar, renomear e excluir playlists; arrastar músicas e pastas; reordenar; pesquisar; ouvir a prévia.
- "Preparando shaders" ao apertar JOGAR, com barra de progresso e botão para pular: tradução, compilação e cache de pipelines.
- Log embutido (ver, copiar, limpar), e o jogo abre sem janela de console.
- Logs enxutos: cada tipo de mensagem repetida aparece só algumas vezes, e cada log guarda só a sessão atual e a anterior (`*.prev.log`).
- **Modo de depuração** (Opções avançadas), desligado por padrão: log completo da bridge e capturas `frame_*.bmp` periódicas da tela.
- Alt+F4 e o botão de fechar da janela fecham o jogo.
- **Atualização automática**: o launcher avisa quando há uma versão nova aqui no GitHub. Se você aceitar, ele baixa o zip, confere o SHA-256, fecha, atualiza a própria pasta e abre de novo. Configurações, saves e shaders são mantidos.

### Estabilidade
- Corrigidos travamentos da GPU por despachos e draws indiretos inválidos (partículas).
- Corrigidos o fechamento ao apertar A na intro, a falha ao carregar o tutorial e travamentos no carregamento e na escolha do veículo.

### Problemas conhecidos
- Fumaça aparece como retângulos com fundo preto, e há placas pretas no chão (inclusive no rastro do rush).
- Blocos pixelados em árvores, água e rochas, e água com reflexo arco-íris em alguns pontos.
- Personagens invisíveis na tela de customização (só aparecem cabelo/contornos).
- Portões verdes do modo Countdown invisíveis (só as setas aparecem).
- Nuvens, poeira na lente (lens dust) e o splash de água dos carros ainda não aparecem.
- Crash ocasional depois do fim de uma corrida.
- Online, multiplayer e cosméticos que dependem do servidor não estão disponíveis.
- Pistas novas podem aparecer com objetos faltando até o jogo ser fechado e os shaders delas serem traduzidos.
