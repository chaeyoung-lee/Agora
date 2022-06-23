use tokio::net::UdpSocket;
use std::io;

static ADDR: &str  = "127.0.0.1:1000";

#[tokio::main]
async fn main() -> io::Result<()> {
    let mut sock = UdpSocket::bind(ADDR).await?;
    let mut buf = [0; 1024];
    loop {
        let (len, addr) = sock.recv_from(&mut buf).await?;
        println!("{:?} bytes received from {:?}", len, addr);

        let len = sock.send_to(&buf[..len], addr).await?;
        println!("{:?} bytes sent", len);
    }
}

async fn request_traffic_data(mut socket: UdpSocket) -> io::Result<()> {
    let message = [0, 0];
    let len = socket.send_to(&message, ADDR).await?;
    println!("{:?} bytes sent", len);
    return Ok(());
}

async fn receive_traffic_data(mut socket: UdpSocket) -> io::Result<()> {
    let mut buf = [0; 100];
    let (len, addr) = socket.recv_from(&mut buf).await?;
    println!("{:?} bytes received from {:?} - content: {:?}", len, addr, &buf[..len]);
    return Ok(());
}

async fn send_control_message(mut socket: UdpSocket) -> io::Result<()> {
    return Ok(());   
}