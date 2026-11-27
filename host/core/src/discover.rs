//! Finding boards that run nocry-rescue on the local network.
//!
//! The board announces `_nocry._tcp` by mDNS (DNS-SD). nocry asks once on every network
//! interface of this computer from an ordinary port. A question that does not come from
//! port 5353 is answered straight back to the sender (RFC 6762 section 6.7), so nocry needs
//! no mDNS service on the computer and does not have to share port 5353 with one. The
//! "unicast answer" bit is deliberately not set: the board's mdnsd (0.12) compares the class
//! field exactly and ignores questions that carry it. The address used is the one the answer
//! came from: that is the address that is known to reach the board.
//!
//! mDNS does not get through every network (guest Wi-Fi, some VPNs), so the board's own
//! access point is also tried directly: it always has the same address.

use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, TcpStream, UdpSocket};
use std::time::{Duration, Instant};

pub const SERVICE: &str = "_nocry._tcp.local";
/// the board's address on its own access point (tools/recovery-build/overlay-rescue)
pub const AP_ADDRESS: Ipv4Addr = Ipv4Addr::new(192, 168, 77, 1);
const MDNS: SocketAddrV4 = SocketAddrV4::new(Ipv4Addr::new(224, 0, 0, 251), 5353);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Board {
    /// the name the board announces, e.g. "nocry-rescue"
    pub name: String,
    pub address: String,
    pub port: u16,
    /// "mdns": it announced itself; "access-point": found at the fixed address of its own Wi-Fi
    pub found_by: &'static str,
}

fn put_name(out: &mut Vec<u8>, name: &str) {
    for label in name.split('.').filter(|l| !l.is_empty()) {
        out.push(label.len() as u8);
        out.extend_from_slice(label.as_bytes());
    }
    out.push(0);
}

/// One question: PTR records of the service.
pub fn query() -> Vec<u8> {
    let mut q = vec![0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0];
    put_name(&mut q, SERVICE);
    q.extend_from_slice(&[0, 12, 0, 1]); // PTR, class IN (no "unicast answer" bit, see above)
    q
}

/// Reads a (possibly compressed) name at `pos`; returns its labels and the position after it.
fn get_name(msg: &[u8], mut pos: usize) -> Option<(Vec<String>, usize)> {
    let mut labels = Vec::new();
    let mut end = None;
    let mut jumps = 0;
    loop {
        let len = *msg.get(pos)? as usize;
        if len == 0 {
            return Some((labels, end.unwrap_or(pos + 1)));
        }
        if len & 0xc0 == 0xc0 {
            let to = ((len & 0x3f) << 8) | *msg.get(pos + 1)? as usize;
            end.get_or_insert(pos + 2);
            jumps += 1;
            if jumps > 16 {
                return None; // a loop of pointers
            }
            pos = to;
            continue;
        }
        if len > 63 {
            return None;
        }
        let l = msg.get(pos + 1..pos + 1 + len)?;
        labels.push(String::from_utf8_lossy(l).into_owned());
        pos += 1 + len;
    }
}

fn same_name(labels: &[String], dotted: &str) -> bool {
    let want: Vec<&str> = dotted.split('.').collect();
    labels.len() == want.len() && labels.iter().zip(&want).all(|(a, b)| a.eq_ignore_ascii_case(b))
}

/// What an mDNS answer says about our service: (instance name, port). None when the packet
/// is not an answer about `_nocry._tcp`.
pub fn parse_answer(msg: &[u8]) -> Option<(String, u16)> {
    if msg.len() < 12 || msg[2] & 0x80 == 0 {
        return None; // not a response
    }
    let count = |i: usize| u16::from_be_bytes([msg[i], msg[i + 1]]) as usize;
    let (questions, records) = (count(4), count(6) + count(8) + count(10));
    let mut pos = 12;
    for _ in 0..questions {
        pos = get_name(msg, pos)?.1 + 4;
    }
    let mut instance: Option<Vec<String>> = None;
    let mut ports: Vec<(Vec<String>, u16)> = Vec::new();
    for _ in 0..records {
        let (owner, p) = get_name(msg, pos)?;
        let head = msg.get(p..p + 10)?;
        let rtype = u16::from_be_bytes([head[0], head[1]]);
        let rlen = u16::from_be_bytes([head[8], head[9]]) as usize;
        let data = p + 10;
        msg.get(data..data + rlen)?;
        match rtype {
            12 if same_name(&owner, SERVICE) => instance = Some(get_name(msg, data)?.0),
            33 if rlen >= 6 => ports.push((owner, u16::from_be_bytes([msg[data + 4], msg[data + 5]]))),
            _ => {}
        }
        pos = data + rlen;
    }
    let instance = instance?;
    let port = ports
        .iter()
        .find(|(owner, _)| owner.len() == instance.len() && owner.iter().zip(&instance).all(|(a, b)| a.eq_ignore_ascii_case(b)))
        .map(|(_, p)| *p)
        .unwrap_or(crate::net::PORT);
    Some((instance.first()?.clone(), port))
}

/// This computer's IPv4 addresses on networks a board could be on.
fn local_addresses() -> Vec<Ipv4Addr> {
    let mut v: Vec<Ipv4Addr> = if_addrs::get_if_addrs()
        .unwrap_or_default()
        .into_iter()
        .filter(|i| !i.is_loopback())
        .filter_map(|i| match i.ip() {
            IpAddr::V4(a) => Some(a),
            _ => None,
        })
        .collect();
    v.sort();
    v.dedup();
    v
}

/// Looks for boards for `wait`. Never fails: a network that cannot be asked gives no boards.
pub fn discover(wait: Duration) -> Vec<Board> {
    let locals = local_addresses();
    let q = query();
    let mut sockets = Vec::new();
    for local in &locals {
        let Ok(s) = UdpSocket::bind(SocketAddrV4::new(*local, 0)) else { continue };
        let s2 = socket2::SockRef::from(&s);
        let _ = s2.set_multicast_if_v4(local);
        let _ = s2.set_multicast_ttl_v4(255);
        let _ = s.set_nonblocking(true);
        if s.send_to(&q, MDNS).is_ok() {
            sockets.push(s);
        }
    }

    // the board's own access point, asked directly while the answers come in
    let on_ap = locals.iter().any(|a| a.octets()[..3] == AP_ADDRESS.octets()[..3]);
    let probe = on_ap.then(|| {
        std::thread::spawn(move || {
            let to = SocketAddr::from((AP_ADDRESS, crate::net::PORT));
            TcpStream::connect_timeout(&to, Duration::from_millis(1500)).is_ok()
        })
    });

    let mut found: Vec<Board> = Vec::new();
    let start = Instant::now();
    let mut asked_again = false;
    let mut buf = [0u8; 2048];
    while start.elapsed() < wait && !sockets.is_empty() {
        // one repeat: the first packet on a Wi-Fi link is the one that gets lost
        if !asked_again && start.elapsed() > wait / 3 {
            asked_again = true;
            for s in &sockets {
                let _ = s.send_to(&q, MDNS);
            }
        }
        let mut got = false;
        for s in &sockets {
            while let Ok((n, from)) = s.recv_from(&mut buf) {
                got = true;
                if let Some((name, port)) = parse_answer(&buf[..n]) {
                    let address = from.ip().to_string();
                    if !found.iter().any(|b| b.address == address) {
                        found.push(Board { name, address, port, found_by: "mdns" });
                    }
                }
            }
        }
        if !got {
            std::thread::sleep(Duration::from_millis(40));
        }
    }
    if let Some(p) = probe {
        let address = AP_ADDRESS.to_string();
        if p.join().unwrap_or(false) && !found.iter().any(|b| b.address == address) {
            found.push(Board { name: "nocry-rescue".into(), address, port: crate::net::PORT, found_by: "access-point" });
        }
    }
    found
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rr(out: &mut Vec<u8>, name: &str, rtype: u16, data: &[u8]) {
        put_name(out, name);
        out.extend_from_slice(&rtype.to_be_bytes());
        out.extend_from_slice(&[0, 1, 0, 0, 0, 120]);
        out.extend_from_slice(&(data.len() as u16).to_be_bytes());
        out.extend_from_slice(data);
    }

    #[test]
    fn query_is_a_plain_ptr_question() {
        let q = query();
        assert_eq!(&q[..12], &[0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
        assert_eq!(get_name(&q, 12).unwrap().0, ["_nocry", "_tcp", "local"]);
        assert_eq!(&q[q.len() - 4..], &[0, 12, 0, 1]);
    }

    #[test]
    fn answer_without_compression() {
        let mut m = vec![0, 0, 0x84, 0, 0, 0, 0, 2, 0, 0, 0, 0];
        let mut inst = Vec::new();
        put_name(&mut inst, "nocry-rescue._nocry._tcp.local");
        rr(&mut m, SERVICE, 12, &inst);
        let mut srv = vec![0, 0, 0, 0, 0x15, 0xb8];
        put_name(&mut srv, "nocry.local");
        rr(&mut m, "nocry-rescue._nocry._tcp.local", 33, &srv);
        assert_eq!(parse_answer(&m), Some(("nocry-rescue".into(), 5560)));
    }

    #[test]
    fn answer_with_pointers_and_no_srv() {
        // header, then the PTR record whose data points back at the owner name
        let mut m = vec![0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0];
        put_name(&mut m, SERVICE); // at offset 12
        m.extend_from_slice(&[0, 12, 0, 1, 0, 0, 0, 120, 0, 8]);
        m.extend_from_slice(&[5, b'b', b'o', b'a', b'r', b'd', 0xc0, 12]);
        assert_eq!(parse_answer(&m), Some(("board".into(), crate::net::PORT)));
    }

    #[test]
    fn answer_of_the_board() {
        // what mdnsd 0.12 on a Nebula Pad running nocry-rescue sent back to this question
        let hex = "000084000001000100000000065f6e6f637279045f746370056c6f63616c00000c0001c00c000c000100000078000f0c6e6f6372792d726573637565c00c";
        let m: Vec<u8> = (0..hex.len() / 2).map(|i| u8::from_str_radix(&hex[2 * i..2 * i + 2], 16).unwrap()).collect();
        assert_eq!(parse_answer(&m), Some(("nocry-rescue".into(), crate::net::PORT)));
    }

    #[test]
    fn other_packets_are_ignored() {
        assert_eq!(parse_answer(&query()), None); // a question, not an answer
        let mut m = vec![0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0];
        rr(&mut m, "_ipp._tcp.local", 12, &[3, b'x', b'y', b'z', 0]);
        assert_eq!(parse_answer(&m), None);
        assert_eq!(parse_answer(&[0, 0, 0x84, 0, 0, 0, 0, 9, 0, 0, 0, 0, 0xc0, 12]), None); // pointer to itself
        assert_eq!(parse_answer(&[1, 2, 3]), None);
    }
}
