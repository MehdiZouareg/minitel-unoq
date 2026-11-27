// =====================================================================
//  UNO Q - cote microcontroleur (STM32U585) : tuyau Minitel <-> Linux
//  + matrice LED en facade pour l'etat de la passerelle.
//
//  Cablage prise DIN du Minitel (D0 = PB7 / D1 = PB6 = USART1 = Serial1) :
//    DIN 1 (RX Minitel)  <-- 1k -- D1 (TX)
//    DIN 2 (masse)       --------- GND
//    DIN 3 (TX Minitel)  --> 1k -- D0 (RX) + pull-up 10k vers 3V3
//
//  1200 bauds sur l'USART1 : le pilote Zephyr ne sait pas y descendre (a 160 MHz, le
//  diviseur BRR de 16 bits deborde), et Serial1.begin(1200) reste alors sans effet.
//  On ouvre donc Serial1 a 9600 bauds, puis on regle nous-memes le prediviseur PRESC
//  du STM32U5 (/8) et BRR pour 1200 bauds. Le materiel cadence les bits : aucune
//  interruption (matrice LED, Bridge...) ne peut les decaler.
//  Le Minitel parle en 7E1 : trame materielle 8N1, parite paire calculee par le code (bit 7).
// =====================================================================
#include <Arduino_RouterBridge.h>
#include <Arduino_LED_Matrix.h>

Arduino_LED_Matrix matrix;

// ---- USART1 du STM32U585 (TrustZone desactivee : alias non securise) ----
#define MT_USART1        0x40013800u
#define MT_REG(off)      (*(volatile uint32_t *)(MT_USART1 + (off)))
#define MT_CR1           MT_REG(0x00)
#define MT_BRR           MT_REG(0x0C)
#define MT_ISR           MT_REG(0x1C)
#define MT_PRESC         MT_REG(0x2C)
#define MT_CR1_UE        (1u << 0)
#define MT_ISR_TC        (1u << 6)
#define MT_PRESC_DIV8    0x4u          // 0100 : horloge noyau / 8

static uint32_t brr9600 = 0;

static void regler1200bauds() {
  brr9600 = MT_BRR & 0xFFFFu;                      // horloge noyau = BRR x 9600 (surechantillonnage x16)
  uint32_t brr1200 = (brr9600 * 9600u / 8u + 600u) / 1200u;
  while (!(MT_ISR & MT_ISR_TC)) {}                 // rien en cours d'emission
  MT_CR1 &= ~MT_CR1_UE;                            // PRESC et BRR ne s'ecrivent qu'USART arretee
  MT_PRESC = MT_PRESC_DIV8;
  MT_BRR = brr1200;
  MT_CR1 |= MT_CR1_UE;
}

// ---- Matrice 13 x 8 -------------------------------------------------
static const char* ICONS[][8] = {
  { // 0 : demarrage  "..."
    ".............", ".............", ".............", ".............",
    "..#...#...#..", ".............", ".............", "............." },
  { // 1 : pret  "M"
    ".............", "...#.....#...", "...##...##...", "...#.#.#.#...",
    "...#..#..#...", "...#.....#...", "...#.....#...", "............." },
  { // 2 : connexion en cours  ">>"
    ".............", "...#....#....", "....#....#...", ".....#....#..",
    "....#....#...", "...#....#....", ".............", "............." },
  { // 3 : en ligne  coche
    ".............", "..........#..", ".........#...", "........#....",
    "..#....#.....", "...#..#......", "....##.......", "............." },
  { // 4 : erreur  "X"
    ".............", "...#.....#...", "....#...#....", ".....#.#.....",
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
  if (dot) frame[0] = 7;               // point d'activite en haut a gauche
  matrix.draw(frame);
}

// ---- File d'emission (remplie par le Bridge, videe au rythme de 1200 bauds) ----
static const uint16_t TX_N = 4096;
static uint8_t  txBuf[TX_N];
static uint16_t txHead = 0, txTail = 0;

static uint16_t txLibre() { return (uint16_t)(TX_N - 1 - (uint16_t)(txHead - txTail) % TX_N); }

uint8_t withEvenParity(uint8_t c) {
  c &= 0x7F;
  uint8_t p = 0;
  for (uint8_t v = c; v; v >>= 1) p ^= (v & 1);
  return p ? (c | 0x80) : c;
}

int hexVal(char h) {
  if (h >= '0' && h <= '9') return h - '0';
  if (h >= 'a' && h <= 'f') return h - 'a' + 10;
  if (h >= 'A' && h <= 'F') return h - 'A' + 10;
  return 0;
}

// ---- Fonctions appelees par Linux (executees dans loop via provide_safe) ----
// Met les octets en file sans bloquer ; renvoie combien ont ete acceptes.
int minitel_write(String hex) {
  int n = 0;
  for (unsigned int i = 0; i + 1 < hex.length(); i += 2) {
    if (txLibre() == 0) break;
    txBuf[txHead % TX_N] = withEvenParity((hexVal(hex[i]) << 4) | hexVal(hex[i + 1]));
    txHead++;
    n++;
  }
  lastActivity = millis();
  return n;
}

int minitel_libre() { return (int)txLibre(); }

String minitel_diag() {
  return String("BRR9600=") + brr9600 + " BRR=" + MT_BRR + " PRESC=" + MT_PRESC + " horloge=" + (brr9600 * 9600u);
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

  Serial1.begin(9600);                 // 8N1, vitesse que le pilote sait regler...
  delay(5);
  regler1200bauds();                   // ...puis 1200 bauds via PRESC

  Bridge.begin();
  Bridge.provide_safe("minitel_write", minitel_write);
  Bridge.provide_safe("minitel_libre", minitel_libre);
  Bridge.provide_safe("minitel_diag", minitel_diag);
  Bridge.provide_safe("set_state", set_state);
}

String rxHex;
uint32_t rxDernier = 0;

void loop() {
  static const char* HX = "0123456789abcdef";
  uint32_t m = millis();

  // Minitel -> Linux, par paquets (fin de rafale ou 64 octets)
  while (Serial1.available() && rxHex.length() < 128) {
    uint8_t b = Serial1.read() & 0x7F;  // on retire le bit de parite
    rxHex += HX[b >> 4];
    rxHex += HX[b & 0x0F];
    rxDernier = m;
  }
  if (rxHex.length() >= 128 || (rxHex.length() > 0 && (int32_t)(m - rxDernier) > 20)) {
    Bridge.notify("from_minitel", rxHex);
    rxHex = "";
    lastActivity = m;
  }

  // Linux -> Minitel : au rythme de 1200 bauds (1 octet / 8,33 ms, rafales de 8 max),
  // pour que le tampon de Serial1 ne se remplisse jamais et que write() ne bloque jamais.
  static uint32_t credit_t = 0;
  static uint8_t  credit = 8;
  uint32_t u = micros();
  if ((uint32_t)(u - credit_t) >= 8334u) {
    uint32_t n = (uint32_t)(u - credit_t) / 8334u;
    credit = (uint8_t)min<uint32_t>(8u, credit + n);
    credit_t += n * 8334u;
  }
  while (credit > 0 && txHead != txTail) {
    Serial1.write(txBuf[txTail % TX_N]);
    txTail++;
    credit--;
  }

  // Point d'activite qui clignote en ligne
  bool dot = (currentIcon == 3) && (m - lastActivity < 150);
  if (dot != activityDot) { activityDot = dot; drawIcon(currentIcon, dot); }
}
