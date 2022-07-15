use tokio::net::UdpSocket;
use tokio::time::{delay_for, Duration};
use std::io;
use std::cmp;
use std::process::Command;

// Communication parameters
static TX_ADDR: &str  = "127.0.0.1:2000";
static RX_ADDR: &str = "127.0.0.1:1000";

// Resource parameters
static MAX_CORE: u8 = 50;
static MIN_CORE: u8 = 10;
static STEPS: u8 = 5;

#[tokio::main]
async fn main() -> io::Result<()> {
    let mut socket = create_udp_socket(RX_ADDR).await?;
    loop {
        delay_for(Duration::from_secs(5)).await; // sends request every 5s
        let traffic = retrieve_agora_traffic(&mut socket).await?; // TODO: implement timeout!
        println!("Agora traffic: {}, current cores: {}\n", traffic[0], traffic[1]);

        if traffic[0] > 10000 {
            let mut add_cores = STEPS;
            if traffic[1] as u8 + add_cores > MAX_CORE {
                add_cores = MAX_CORE - traffic[1] as u8;
            }

            if add_cores > 0 {
                println!("Too much traffic: add {:?} cores\n", add_cores);
                send_control_message(&mut socket, STEPS, 0, traffic[1] as u8).await?;
            }
        } else if traffic[0] < 5000 {
            let mut remove_cores = STEPS;
            if traffic[1] as u8 - remove_cores < MIN_CORE {
                remove_cores = traffic[1] as u8 - MIN_CORE;
            }

            if remove_cores > 0 {
                println!("Too relaxed traffic: remove {:?} cores\n", remove_cores);
                send_control_message(&mut socket, 0, remove_cores, traffic[1] as u8).await?;
            }
        }
    }
}

async fn create_udp_socket(host: &str) -> io::Result<UdpSocket> {
    println!("Creating socket on {}", host);
    let socket = UdpSocket::bind(&host).await?;
    return Ok(socket);
}

/* UDP Functions */

// Send message through UDP socket
async fn send_message(socket: &mut UdpSocket, message: &[u8; 16]) -> io::Result<()> {
    let len = socket.send_to(message, TX_ADDR).await?;
    println!("{:?} bytes sent", len);
    return Ok(());
}

// Listen message on UDP socket
async fn listen_message(socket: &mut UdpSocket) -> io::Result<[u64; 2]> {
    let mut buf = [0; 100];
    let (len, addr) = socket.recv_from(&mut buf).await?;
    let a: u64 = u64::from_ne_bytes(buf[..8].try_into().unwrap());
    let b: u64 = u64::from_ne_bytes(buf[8..len].try_into().unwrap());
    let message = [a, b];
    println!("{:?} bytes received from {:?} - content: {:?}", len, addr, &message);
    return Ok(message);
}

/* Resource Provisioner Logic */

async fn retrieve_agora_traffic(socket: &mut UdpSocket) -> io::Result<[u64; 2]> {
    let message = [0; 16];
    send_message(socket, &message).await?;
    return listen_message(socket).await;
}

async fn send_control_message(socket: &mut UdpSocket, add_cores: u8, remove_cores: u8, curr_cores: u8) -> io::Result<()> {
    let mut message = [0; 16];
    message[0] = add_cores;
    message[8] = remove_cores;

    if add_cores > remove_cores {
        // ask for cores
       let result = request_resource_update(curr_cores, add_cores, remove_cores).await?;
       delay_for(Duration::from_secs(5)).await; // TODO: intelligent check
       if result {
        // allow agora to add workers
        send_message(socket, &message).await?;
        println!("Agora workers: added");
       }
    } else {
        // allow agora to remove workers
        send_message(socket, &message).await?;
        loop { // wait until worker has been removed
            let traffic = retrieve_agora_traffic(socket).await?;
            if traffic[1] <= (curr_cores + add_cores - remove_cores) as u64 {
                println!("Agora workers: removed");
                break;
            }
        }
        let result = request_resource_update(curr_cores, add_cores, remove_cores).await?;
    }

    delay_for(Duration::from_secs(1)).await; // give time for Agora to process request TODO: more intelligent way?
    return Ok(());
}

/* Communicate with Docker */

async fn request_resource_update(curr_cores: u8, add_cores: u8, remove_cores: u8) -> io::Result<bool> {
    let num = curr_cores + add_cores - remove_cores;
    let output = Command::new("docker")
                                    .arg("update")
                                    .arg(format!("--cpuset-cpus=0-{}", num))
                                    .arg("agora_net")
                                    .output()
                                    .expect("Docker: Failed to execute process\n");
    return Ok(output.status.success());
}