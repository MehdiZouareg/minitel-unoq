// =====================================================================
//  UNO Q - cote microcontroleur (STM32) : tuyau transparent Minitel <-> Linux
//  + matrice LED en facade pour l'etat de la passerelle.
//
//  Cablage prise DIN du Minitel :
//    DIN 1 (RX Minitel)  <-- 1k -- D1
//    DIN 2 (masse)       --------- GND
//    DIN 3 (TX Minitel)  --> 1k -- D0 + pull-up 10k vers 3V3
//
//  D0/D1 sont l'USART1 = console Zephyr a 115200, que le pilote ne sait pas
//  descendre a 1200 bauds. On fait donc une UART LOGICIELLE 1200 bauds 7E1 :
//   - emission : bits generes sur D1 avec des echeances absolues en micros()
//   - reception : interruption sur chaque front de D0 (horodatage), puis
//     decodage des trames dans loop() a partir des horodatages.
// =====================================================================
#include <Arduino_LED_Matrix.h>
#include <Arduino_RouterBridge.h>

Arduino_LED_Matrix matrix;

static const int PIN_RX = 0;      // D0 <- Minitel
static const int PIN_TX = 1;      // D1 -> Minitel
static const uint32_t BIT_US = 833;   // 1 / 1200 s

// ---- Matrice 13 x 8 -------------------------------------------------
static const char* ICONS[][8] = {
  { ".............", ".............", ".............", ".............",
    "..#...#...#..", ".............", ".............", "............." },
  { ".............", "...#.....#...", "...##...##...", "...#.#.#.#...",
    "...#..#..#...", "...#.....#...", "...#.....#...", "............." },
  { ".............", "...#....#....", "....#....#...", ".....#....#..",
    "....#....#...", "...#....#....", ".............", "............." },
  { ".............", "..........#..", ".........#...", "........#....",
    "..#....#.....", "...#..#......", "....##.......", "............." },
  { ".............", "...#.....#...", "....#...#....", ".....#.#.....",
    "......#......", ".....#.#.....", "....#...#....", "...#.....#..." },
};

uint8_t frame[104];
int currentIcon = 0;
uint32_t lastActivity = 0;
bool activityDot = false;

void drawIcon(int idx, bool dot) {
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 13; x++)
      frame[y * 13 + x] = ICONS[idx][y][x] == '#' ? 7 : 0;
  if (dot) frame[0] = 7;
  matrix.draw(frame);
}

// ---- UART logicielle : emission --------------------------------------
uint8_t withEvenParity(uint8_t c) {
  c &= 0x7F;
  uint8_t p = 0;
  for (uint8_t v = c; v; v >>= 1) p ^= (v & 1);
  return p ? (c | 0x80) : c;
}

void txByte(uint8_t b) {                 // b = 8 bits deja prets (7 donnees + parite)
  uint16_t f = ((uint16_t)b << 1) | (1u << 9);   // start(0) + 8 bits + stop(1)
  uint32_t t0 = micros();
  digitalWrite(PIN_TX, LOW);
  for (int i = 1; i < 10; i++) {
    while ((uint32_t)(micros() - t0) < BIT_US * i) {}
    digitalWrite(PIN_TX, (f >> i) & 1);
  }
  while ((uint32_t)(micros() - t0) < BIT_US * 10 + 100) {}   // stop + petite marge
}

// ---- UART logicielle : reception --------------------------------------
static const uint16_t NE = 512;
volatile uint32_t edgeT[NE];
volatile uint8_t  edgeL[NE];
volatile uint16_t edgeHead = 0;
uint16_t edgeTail = 0;

void onEdge() {
  uint16_t h = edgeHead;
  edgeT[h % NE] = micros();
  edgeL[h % NE] = digitalRead(PIN_RX);
  edgeHead = h + 1;
}

bool inFrame = false;
uint32_t frameT0 = 0;
String rxHex;

void emitByte(uint8_t v) {
  const char* d = "0123456789abcdef";
  rxHex += d[v >> 4];
  rxHex += d[v & 0x0F];
}

void decodeRx() {
  while (true) {
    if (!inFrame) {
      if (edgeTail == edgeHead) return;
      uint16_t k = edgeTail % NE;
      if (edgeL[k] == 0) { inFrame = true; frameT0 = edgeT[k]; }
      edgeTail++;
      continue;
    }
    if ((uint32_t)(micros() - frameT0) < BIT_US * 10) return;   // trame pas finie
    uint8_t v = 0; uint8_t level = 0; uint16_t k = edgeTail;
    for (int i = 1; i <= 8; i++) {
      uint32_t ts = frameT0 + BIT_US * i + BIT_US / 2;
      while (k != edgeHead && (int32_t)(edgeT[k % NE] - ts) <= 0) { level = edgeL[k % NE]; k++; }
      if (level) v |= (1 << (i - 1));
    }
    uint32_t tEnd = frameT0 + BIT_US * 9 + BIT_US / 2;
    while (edgeTail != edgeHead && (int32_t)(edgeT[edgeTail % NE] - tEnd) <= 0) edgeTail++;
    inFrame = false;
    emitByte(v & 0x7F);                  // on retire la parite
  }
}

// ---- Fonctions appelees par Linux (executees dans loop via provide_safe) ----
int hexVal(char h) {
  if (h >= '0' && h <= '9') return h - '0';
  if (h >= 'a' && h <= 'f') return h - 'a' + 10;
  if (h >= 'A' && h <= 'F') return h - 'A' + 10;
  return 0;
}

bool minitel_write(String hex) {
  for (unsigned int i = 0; i + 1 < hex.length(); i += 2)
    txByte(withEvenParity((hexVal(hex[i]) << 4) | hexVal(hex[i + 1])));
  lastActivity = millis();
  return true;
}

bool set_state(int idx) {
  if (idx < 0 || idx > 4) return false;
  currentIcon = idx;
  drawIcon(currentIcon, false);
  return true;
}

void setup() {
  matrix.begin();
  matrix.setGrayscaleBits(3);
  matrix.clear();
  drawIcon(0, false);

  pinMode(PIN_TX, OUTPUT);
  digitalWrite(PIN_TX, HIGH);            // ligne au repos = 1
  pinMode(PIN_RX, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_RX), onEdge, CHANGE);

  Bridge.begin();
  Bridge.provide_safe("minitel_write", minitel_write);
  Bridge.provide_safe("set_state", set_state);
}

uint32_t lastFlush = 0;

void loop() {
  decodeRx();
  // Envoie les octets recus par paquets (fin de rafale ou 64 octets)
  if (rxHex.length() >= 128 || (rxHex.length() > 0 && !inFrame && millis() - lastFlush > 30)) {
    Bridge.notify("from_minitel", rxHex);
    rxHex = "";
    lastActivity = millis();
  }
  if (rxHex.length() == 0) lastFlush = millis();

  bool dot = (currentIcon == 3) && (millis() - lastActivity < 150);
  if (dot != activityDot) { activityDot = dot; drawIcon(currentIcon, dot); }
}
