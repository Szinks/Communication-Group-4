#include <stdio.h>  //This library provides functions like  printf(), sscanf(), and snprintf().
#include <libpynq.h>  //This library provides functions  for UART, switchbox, and display
#include <stdint.h>  //This library provides fixed-size integer types such as uint8_t
#include <string.h>  //This library provides functions for working with strings.

int main(void)
{
    pynq_init();  //Initializes the PYNQ hardware and the libpynq library.

    switchbox_set_pin(IO_AR0, SWB_UART0_RX);  //Connects physical pin AR0 to the RX input of UART0. UART0 is used to receive the JSON message from the Mainframe.
    switchbox_set_pin(IO_AR3, SWB_UART1_TX);  //Connects physical pin AR3 to the TX output of UART1. UART1 is used to send the processed packet to the Tracking Submodule.

    uart_init(UART0); // Initializes UART0 for receiving data from the Mainframe.
    uart_init(UART1); //Initializes UART1 for transmitting data to Tracking.

    uart_reset_fifos(UART0);  //Clears the UART0 FIFO buffers. This prevents old or unwanted bytes from being interpreted as new input.
    uart_reset_fifos(UART1); //Clears the UART1 FIFO buffers before transmission starts.

    display_t display;  //This Creates a display structure that is used to control the LCD.
    display_init(&display);  // Initializes the LCD and passes the address of the display structure to the function.

    FontxFile font[2];  //Creates space for the font information used by the LCD.
    InitFontx(font, "../../fonts/ILGH16XB.FNT", ""); //Loads the selected font file that will be used to display text.
    displaySetFontDirection(&display, TEXT_DIRECTION0); //Sets the text direction to the normal horizontal direction.

    char input[600];
    char id[32];
    char payload[64];
    long long timestamp;
    float x;
    float y;
    //Declaration of variables that are in the JSON code plus a string called input

    printf("Waiting for the mainframe...\n");    //Prints a status message once when the program starts.

    while (1)
    {
        int i = 0;
        int brackets = 0; // Variable brackets is a counter that counts the '{' and '}' characters to detect when the complete JSON message has been received.
        int start = 0;

        while (i < 599)
        {
            char c = uart_recv(UART0);   // Receives one character from the Mainframe through UART0 and stores it in c.

            if (start == 0)
            {
                if (c != '{')
                    continue;
                      //Ignores incoming characters until the first '{' marks the beginning of the JSON code.
                start = 1;  //The beginning of the JSON code has now been found.
            }

            input[i++] = c;  // Stores the received character in input[i] and then increases i to point to the next free position.

            if (c == '{')
                brackets++;  // Increases the counter every time an opening curly bracket is received.

            if (c == '}')
                brackets--;  // Decreases the counter every time a closing curly bracket is received.

            if (start && brackets == 0) // Checks that the JSON has started and that the full message has been received.
            {
                input[i] = '\0'; //Adds '\0' after the last received character so input becomes a complete C string.
                break;
            }
        }

     int parse = sscanf(input, " { \"ping\" : { \"id\" : \"%31[^\"]\" , \"timestamp_ms\" : %lld , \"payload\" : \"%63[^\"]\" } , \"coordinates\" : { \"X\" : %f , \"Y\" : %f } } ", id, &timestamp, payload, &x, &y);
     // The function sscanf reads the received JSON string and stores the extracted values in the corresponding variables: id, timestamp, payload, x, and y.   

         if (parse != 5)
            continue; 
        // If all five values cannot be extracted from the received JSON, the entire JSON message is skipped.

        printf("Message: %s | X: %.2f | Y: %.2f\n", payload, x, y); // Prints the received message and coordinates in the terminal for checking.

        char binary[1000];
        int position = 0;

        for (int j = 0; payload[j] != '\0'; j++)
        {
            unsigned char c = payload[j];  //Takes one payload character at a time so its 7-bit ASCII value can be examined bit by bit.

            binary[position++] = '0'; // Adds a leading 0 before the seven ASCII data bits of the current character.

            for (int bit = 6; bit >= 0; bit--) //Goes through the 7 ASCII bits from bit 6 to bit 0, from left to right.
            {
                if ((c >> bit) & 1) //Shifts the selected bit to the rightmost position and checks whether it is 1.
                    binary[position++] = '1'; // Stores '1' in the binary array if the selected bit is 1.
                else  // Runs when the selected bit is 0.
                    binary[position++] = '0'; // Stores '0' in the binary array if the selected bit is 0.
            }

            if (payload[j + 1] == '\0') // Checks if the current payload character is the last one.
                binary[position++] = '1';  // Adds control bit 1 after the 7 ASCII bits for the last character.
            else
                binary[position++] = '0'; // Adds control bit 0 after the 7 ASCII bits if another character follows.
        }

        binary[position] = '\0'; // Adds '\0' after all binary characters to mark the end of the binary string.

        char output_data[1200];

        snprintf(output_data, sizeof(output_data), "%lld,%s,%.2f,%.2f\n", timestamp, binary, x, y); 
         // Combines timestamp, binary message, X, and Y into one string and stores it in output_data; sizeof(output_data) prevents writing beyond the array size.

        for (int j = 0; output_data[j] != '\0'; j++)
        {
            uart_send(UART1, output_data[j]);  // Sends the complete packet to Tracking through UART1 one character at a time.
        }

        char line1[80];
        char line2[50];
        char line3[40];
        char line4[40];

        snprintf(line1, sizeof(line1), "Message: %.18s", payload);  // Creates the message line for the LCD, stores it in line1, and limits writing to the size of line1.
        snprintf(line2, sizeof(line2), "Time: %lld", timestamp);    // Creates the timestamp line for the LCD, stores it in line2.
        snprintf(line3, sizeof(line3), "X: %.2f", x);  // Creates the X-coordinate line for the LCD, stores it in line3.
        snprintf(line4, sizeof(line4), "Y: %.2f", y);  // Creates the Y-coordinate line for the LCD, stores it in line4.

        displayFillScreen(&display, RGB_BLACK);  // Clears the previous LCD contents before displaying the new data.

        displayDrawString(&display, font, 5, 30, (uint8_t *)line1, RGB_WHITE);  // Displays line1 on the LCD at position (5,30); uint8_t* passes the text address in the type expected by the display function.
        displayDrawString(&display, font, 5, 65, (uint8_t *)line2, RGB_WHITE);  // Displays line2 on the LCD at position (5,65); uint8_t* passes the text address in the type expected by the display function.
        displayDrawString(&display, font, 5, 100, (uint8_t *)line3, RGB_WHITE); // Displays line3 on the LCD at position (5,100); uint8_t* passes the text address in the type expected by the display function.
        displayDrawString(&display, font, 5, 135, (uint8_t *)line4, RGB_WHITE); // Displays line4 on the LCD at position (5,135); uint8_t* passes the text address in the type expected by the display function.
    }

    return 0;
}