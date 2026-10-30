#include <Arduino.h>
#include <MFRC522.h>
#include <SPI.h>

#include "board_config.h"
#include "rfid.h"

namespace
{
    constexpr uint8_t RFID_RST_PIN = 0xFF;

    MFRC522 rfid1(RFID1_GPIO_SS, RFID_RST_PIN);
    MFRC522 rfid2(RFID2_GPIO_SS, RFID_RST_PIN);
    MFRC522 rfid3(RFID3_GPIO_SS, RFID_RST_PIN);

    void scan_reader(MFRC522 &reader, const char *name)
    {
        if (reader.PICC_IsNewCardPresent() && reader.PICC_ReadCardSerial())
        {
            Serial.printf("%s UID: ", name);
            for (byte i = 0; i < reader.uid.size; ++i)
            {
                if (reader.uid.uidByte[i] < 0x10)
                {
                    Serial.print('0');
                }
                Serial.print(reader.uid.uidByte[i], HEX);
            }
            Serial.println();

            reader.PICC_HaltA();
            reader.PCD_StopCrypto1();
        }
    }
}

void rfid_init()
{
    SPI.begin(RFID_GPIO_SCK, RFID_GPIO_MISO, RFID_GPIO_MOSI, RFID1_GPIO_SS);
    rfid1.PCD_Init();
    rfid2.PCD_Init();
    rfid3.PCD_Init();

    Serial.println("Reader 1 version:");
    rfid1.PCD_DumpVersionToSerial();
    Serial.println("Reader 2 version:");
    rfid2.PCD_DumpVersionToSerial();
    Serial.println("Reader 3 version:");
    rfid3.PCD_DumpVersionToSerial();
    Serial.println("RFID readers ready; scan a card or tag");
}

void rfid_poll()
{
    scan_reader(rfid1, "Reader 1");
    scan_reader(rfid2, "Reader 2");
    scan_reader(rfid3, "Reader 3");
}
