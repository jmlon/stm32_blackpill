/*
  Serial

  Interactive command shell over the USB-C port (USB CDC, /dev/ttyACM0).
  Open it with the "Serial monitor" Zed task or:

      picocom -b 115200 /dev/ttyACM0

  Commands:
    help               list commands
    led on|off|toggle  drive the on-board LED (PC13)
    blink [ms]         blink the LED with the given half-period (0 stops)
    uptime             time since reset
    info               chip and clock details

  Characters are echoed back (picocom does no local echo) and backspace
  works, so the shell is usable by hand.
*/

// The Black Pill LED on PC13 is wired active-low.
const uint8_t LED_ON = LOW;
const uint8_t LED_OFF = HIGH;

const size_t LINE_MAX = 64;
char line[LINE_MAX];
size_t lineLen = 0;

bool ledState = false;
unsigned long blinkPeriod = 0;  // 0: blinking disabled
unsigned long lastToggle = 0;

void setLed(bool on) {
  ledState = on;
  digitalWrite(LED_BUILTIN, on ? LED_ON : LED_OFF);
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  help               list commands"));
  Serial.println(F("  led on|off|toggle  drive the on-board LED"));
  Serial.println(F("  blink [ms]         blink the LED (0 stops)"));
  Serial.println(F("  uptime             time since reset"));
  Serial.println(F("  info               chip and clock details"));
}

void printUptime() {
  unsigned long s = millis() / 1000;
  Serial.printf("Uptime: %lud %02lu:%02lu:%02lu\r\n",
                s / 86400, (s / 3600) % 24, (s / 60) % 60, s % 60);
}

void printInfo() {
  // Unique device ID (96 bits) and flash size register, see RM0383 §24.
  const uint32_t *uid = (const uint32_t *)UID_BASE;
  const uint16_t flashKb = *(const uint16_t *)FLASHSIZE_BASE;

  Serial.println(F("Board: WeAct Black Pill (STM32F411CEU6)"));
  Serial.printf("Core clock: %lu MHz\r\n", SystemCoreClock / 1000000);
  Serial.printf("Flash: %u KB\r\n", flashKb);
  Serial.printf("UID: %08lX%08lX%08lX\r\n", uid[2], uid[1], uid[0]);
}

void handleCommand(char *cmd) {
  char *verb = strtok(cmd, " ");
  char *arg = strtok(nullptr, " ");
  if (verb == nullptr) {
    return;  // empty line
  }

  if (strcmp(verb, "help") == 0) {
    printHelp();
  } else if (strcmp(verb, "led") == 0) {
    blinkPeriod = 0;
    if (arg && strcmp(arg, "on") == 0) {
      setLed(true);
    } else if (arg && strcmp(arg, "off") == 0) {
      setLed(false);
    } else if (arg && strcmp(arg, "toggle") == 0) {
      setLed(!ledState);
    } else {
      Serial.println(F("Usage: led on|off|toggle"));
      return;
    }
    Serial.println(ledState ? F("LED on") : F("LED off"));
  } else if (strcmp(verb, "blink") == 0) {
    blinkPeriod = arg ? strtoul(arg, nullptr, 10) : 500;
    if (blinkPeriod == 0) {
      setLed(false);
      Serial.println(F("Blinking stopped"));
    } else {
      Serial.printf("Blinking every %lu ms\r\n", blinkPeriod);
    }
  } else if (strcmp(verb, "uptime") == 0) {
    printUptime();
  } else if (strcmp(verb, "info") == 0) {
    printInfo();
  } else {
    Serial.printf("Unknown command '%s', try 'help'\r\n", verb);
  }
}

void prompt() {
  Serial.print(F("> "));
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\r' || c == '\n') {
      // Treat CR, LF and CRLF alike: ignore the empty line a CRLF would leave.
      if (c == '\n' && lineLen == 0) {
        continue;
      }
      Serial.println();
      line[lineLen] = '\0';
      handleCommand(line);
      lineLen = 0;
      prompt();
    } else if (c == '\b' || c == 0x7f) {  // backspace / delete
      if (lineLen > 0) {
        lineLen--;
        Serial.print(F("\b \b"));
      }
    } else if (isprint(c) && lineLen < LINE_MAX - 1) {
      line[lineLen++] = c;
      Serial.print(c);
    }
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  setLed(false);

  Serial.begin(115200);  // baud rate is ignored for USB CDC

  // Wait up to 5 s for a terminal to open the port so the banner isn't lost;
  // the LED flickers meanwhile. Without a host the sketch starts anyway.
  unsigned long start = millis();
  while (!Serial && millis() - start < 5000) {
    setLed(!ledState);
    delay(50);
  }
  setLed(false);

  Serial.println();
  Serial.println(F("Black Pill serial demo"));
  printHelp();
  prompt();
}

void loop() {
  readSerial();

  if (blinkPeriod > 0 && millis() - lastToggle >= blinkPeriod) {
    lastToggle = millis();
    setLed(!ledState);
  }
}
