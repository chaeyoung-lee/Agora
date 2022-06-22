import socket
import sys
import struct
import time

ip = "127.0.0.2"
rx_port = 7777 # Agora -> RP
tx_port = 7777 # Agora <- RP

# Create a UDP socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
# Bind the socket to the port
server_address = (ip, rx_port)
s.bind(server_address)
print("Do Ctrl+c to exit the program !!")

# Send data
send_data = struct.pack('NN', 1, 2)
s.sendto(send_data, (ip, tx_port))
print("\n\n Server sent data\n\n")

while True:
    # Receive data
    print("####### Server is listening #######")
    data, address = s.recvfrom(4096)
    decoded_data = struct.unpack('NN', data) # 2 size_t type
    print("\n\n Server received: ", decoded_data, "\n\n")

    # Send data
    num = input("# cores?: ").split(',')
    send_data = struct.pack('NN', int(num[0]), int(num[1]))
    s.sendto(send_data, (ip, tx_port))
    print("\n\n Server sent data\n\n")

    # timer delay for 1 sec
    time.sleep(1)