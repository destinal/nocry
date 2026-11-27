//! Control over the LAN: the link between nocry and `nocry-netd` on a board running
//! nocry-rescue (docs/nocry-rescue.md).
//!
//! Pairing is SPAKE2 (RFC 9382, P-256, SHA-256, HKDF, HMAC) with the short code the board
//! shows on its screen as the password: a wrong code gives an attacker one guess per
//! connection and nothing to test guesses against afterwards. Both sides then confirm the
//! key and switch to ChaCha20-Poly1305 records.
//!
//!   client -> board   "NOCRY-NET 1\n", pA (65 bytes)
//!   board -> client   pB (65 bytes), cB (32 bytes)
//!   client -> board   cA (32 bytes)
//!   then records, each: u32 LE length of the ciphertext, ciphertext with its tag
//!
//! Inside the records runs the stream that `nocry-fbd --stdio` speaks: commands and answers
//! as packets with a u32 LE length, data phases raw.

use anyhow::{anyhow, bail, Context, Result};
use chacha20poly1305::aead::{Aead, KeyInit};
use chacha20poly1305::{ChaCha20Poly1305, Nonce};
use hmac::{Hmac, Mac};
use p256::elliptic_curve::ops::Reduce;
use p256::elliptic_curve::sec1::{FromEncodedPoint, ToEncodedPoint};
use p256::elliptic_curve::Field;
use p256::{AffinePoint, EncodedPoint, ProjectivePoint, Scalar, U256};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

pub const PORT: u16 = 5560;
pub const HELLO: &[u8] = b"NOCRY-NET 1\n";
pub const ID_CLIENT: &[u8] = b"nocry";
pub const ID_BOARD: &[u8] = b"nocry-rescue";
/// plaintext bytes per record
pub const RECORD: usize = 64 * 1024;

const M_HEX: &str = "02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f";
const N_HEX: &str = "03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49";

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap()).collect()
}

fn point(bytes: &[u8]) -> Result<ProjectivePoint> {
    let e = EncodedPoint::from_bytes(bytes).map_err(|_| anyhow!("bad curve point"))?;
    let a: Option<AffinePoint> = AffinePoint::from_encoded_point(&e).into();
    let p = ProjectivePoint::from(a.ok_or_else(|| anyhow!("point is not on the curve"))?);
    if p == ProjectivePoint::IDENTITY {
        bail!("point at infinity");
    }
    Ok(p)
}

fn encode(p: &ProjectivePoint) -> Vec<u8> {
    p.to_affine().to_encoded_point(false).as_bytes().to_vec()
}

/// The pairing code as the SPAKE2 password scalar: SHA-256 of a label and the code, reduced.
pub fn code_scalar(code: &str) -> Scalar {
    let mut h = Sha256::new();
    h.update(b"nocry-rescue pairing code\0");
    h.update(code.trim().as_bytes());
    <Scalar as Reduce<U256>>::reduce_bytes(&h.finalize())
}

/// Keys that come out of a finished exchange.
pub struct Keys {
    pub ke: [u8; 16],
    pub kc_a: [u8; 16],
    pub kc_b: [u8; 16],
    pub tt: Vec<u8>,
}

impl Keys {
    pub fn confirm_a(&self) -> [u8; 32] {
        mac(&self.kc_a, &self.tt)
    }
    pub fn confirm_b(&self) -> [u8; 32] {
        mac(&self.kc_b, &self.tt)
    }
    /// 32-byte record key for one direction ("c2b" or "b2c").
    pub fn record_key(&self, direction: &str) -> [u8; 32] {
        let mut k = [0u8; 32];
        hkdf::Hkdf::<Sha256>::new(None, &self.ke).expand(format!("nocry-net {direction}").as_bytes(), &mut k).unwrap();
        k
    }
}

fn mac(key: &[u8], msg: &[u8]) -> [u8; 32] {
    let mut m = <Hmac<Sha256> as Mac>::new_from_slice(key).unwrap();
    m.update(msg);
    m.finalize().into_bytes().into()
}

fn transcript(id_a: &[u8], id_b: &[u8], pa: &[u8], pb: &[u8], k: &[u8], w: &Scalar) -> Vec<u8> {
    let mut tt = Vec::new();
    for part in [id_a, id_b, pa, pb, k, w.to_bytes().as_slice()] {
        tt.extend_from_slice(&(part.len() as u64).to_le_bytes());
        tt.extend_from_slice(part);
    }
    tt
}

fn keys(tt: Vec<u8>) -> Keys {
    let h = Sha256::digest(&tt);
    let (ke, ka) = h.split_at(16);
    let mut kc = [0u8; 32];
    hkdf::Hkdf::<Sha256>::new(None, ka).expand(b"ConfirmationKeys", &mut kc).unwrap();
    Keys { ke: ke.try_into().unwrap(), kc_a: kc[..16].try_into().unwrap(), kc_b: kc[16..].try_into().unwrap(), tt }
}

/// Side A (the client): first message, and the keys once B's message is in.
pub struct SideA {
    x: Scalar,
    w: Scalar,
    pub pa: Vec<u8>,
}

impl SideA {
    pub fn start(w: Scalar) -> Result<Self> {
        Self::start_with(w, Scalar::random(&mut rand_core()))
    }
    pub fn start_with(w: Scalar, x: Scalar) -> Result<Self> {
        let m = point(&unhex(M_HEX))?;
        let pa = encode(&(ProjectivePoint::GENERATOR * x + m * w));
        Ok(SideA { x, w, pa })
    }
    pub fn finish(&self, id_a: &[u8], id_b: &[u8], pb: &[u8]) -> Result<Keys> {
        let n = point(&unhex(N_HEX))?;
        let k = (point(pb)? - n * self.w) * self.x;
        if k == ProjectivePoint::IDENTITY {
            bail!("bad key share");
        }
        Ok(keys(transcript(id_a, id_b, &self.pa, pb, &encode(&k), &self.w)))
    }
}

/// Side B (the board), here for the tests: nocry-netd is the real one.
pub fn side_b(w: Scalar, y: Scalar, id_a: &[u8], id_b: &[u8], pa: &[u8]) -> Result<(Vec<u8>, Keys)> {
    let (m, n) = (point(&unhex(M_HEX))?, point(&unhex(N_HEX))?);
    let pb = encode(&(ProjectivePoint::GENERATOR * y + n * w));
    let k = (point(pa)? - m * w) * y;
    Ok((pb.clone(), keys(transcript(id_a, id_b, pa, &pb, &encode(&k), &w))))
}

struct Os;
impl p256::elliptic_curve::rand_core::RngCore for Os {
    fn next_u32(&mut self) -> u32 {
        let mut b = [0u8; 4];
        self.fill_bytes(&mut b);
        u32::from_le_bytes(b)
    }
    fn next_u64(&mut self) -> u64 {
        let mut b = [0u8; 8];
        self.fill_bytes(&mut b);
        u64::from_le_bytes(b)
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        getrandom::getrandom(dest).expect("no random numbers from the system");
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> std::result::Result<(), p256::elliptic_curve::rand_core::Error> {
        self.fill_bytes(dest);
        Ok(())
    }
}
impl p256::elliptic_curve::rand_core::CryptoRng for Os {}
fn rand_core() -> Os {
    Os
}

/// The encrypted link, after pairing.
pub struct Link {
    s: TcpStream,
    tx: ChaCha20Poly1305,
    rx: ChaCha20Poly1305,
    tx_n: u64,
    rx_n: u64,
    /// decrypted bytes not yet handed out
    buf: Vec<u8>,
    pos: usize,
    pub peer: String,
}

fn nonce(n: u64) -> Nonce {
    let mut b = [0u8; 12];
    b[4..].copy_from_slice(&n.to_le_bytes());
    *Nonce::from_slice(&b)
}

impl Link {
    /// Connects to `addr` ("host" or "host:port") and pairs with `code`.
    pub fn connect(addr: &str, code: &str) -> Result<Link> {
        let target = if addr.contains(':') && !addr.starts_with('[') || addr.contains("]:") { addr.to_string() } else { format!("{addr}:{PORT}") };
        let sa = target.to_socket_addrs().with_context(|| format!("looking up {target}"))?.next().ok_or_else(|| anyhow!("{target}: no address"))?;
        let mut s = TcpStream::connect_timeout(&sa, Duration::from_secs(8)).with_context(|| format!("connecting to {target}"))?;
        s.set_nodelay(true).ok();
        s.set_read_timeout(Some(Duration::from_secs(20)))?;
        s.set_write_timeout(Some(Duration::from_secs(20)))?;
        let a = SideA::start(code_scalar(code))?;
        s.write_all(HELLO)?;
        s.write_all(&a.pa)?;
        let mut pb = [0u8; 65];
        let mut cb = [0u8; 32];
        s.read_exact(&mut pb).context("the board closed the connection during pairing (another computer connected, or too many wrong codes?)")?;
        s.read_exact(&mut cb).context("the board closed the connection during pairing")?;
        let k = a.finish(ID_CLIENT, ID_BOARD, &pb)?;
        // constant-time enough for a one-shot check: compare MACs of the two values
        if mac(&k.ke, &cb) != mac(&k.ke, &k.confirm_b()) {
            bail!("the pairing code is not the one the board shows");
        }
        s.write_all(&k.confirm_a())?;
        s.set_read_timeout(Some(Duration::from_secs(120)))?;
        s.set_write_timeout(Some(Duration::from_secs(120)))?;
        Ok(Link {
            s,
            tx: ChaCha20Poly1305::new_from_slice(&k.record_key("c2b")).unwrap(),
            rx: ChaCha20Poly1305::new_from_slice(&k.record_key("b2c")).unwrap(),
            tx_n: 0,
            rx_n: 0,
            buf: Vec::new(),
            pos: 0,
            peer: target,
        })
    }

    pub fn set_timeout(&mut self, t: Duration) {
        let _ = self.s.set_read_timeout(Some(t));
    }

    /// Sends bytes of the inner stream.
    pub fn send(&mut self, data: &[u8]) -> Result<()> {
        for c in data.chunks(RECORD) {
            let ct = self.tx.encrypt(&nonce(self.tx_n), c).map_err(|_| anyhow!("encryption failed"))?;
            self.tx_n += 1;
            self.s.write_all(&(ct.len() as u32).to_le_bytes())?;
            self.s.write_all(&ct)?;
        }
        Ok(())
    }

    fn fill(&mut self) -> Result<()> {
        let mut l = [0u8; 4];
        self.s.read_exact(&mut l).context("reading from the board over the network")?;
        let n = u32::from_le_bytes(l) as usize;
        if n < 16 || n > RECORD + 16 {
            bail!("the board sent a record of impossible size");
        }
        let mut ct = vec![0u8; n];
        self.s.read_exact(&mut ct).context("reading from the board over the network")?;
        self.buf = self.rx.decrypt(&nonce(self.rx_n), ct.as_ref()).map_err(|_| anyhow!("a record from the board failed its check; the connection is not safe to use"))?;
        self.rx_n += 1;
        self.pos = 0;
        Ok(())
    }

    /// Receives exactly `out.len()` bytes of the inner stream.
    pub fn recv_exact(&mut self, out: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < out.len() {
            if self.pos == self.buf.len() {
                self.fill()?;
            }
            let n = (out.len() - done).min(self.buf.len() - self.pos);
            out[done..done + n].copy_from_slice(&self.buf[self.pos..self.pos + n]);
            self.pos += n;
            done += n;
        }
        Ok(())
    }

    /// One command or answer packet of the inner stream.
    pub fn send_packet(&mut self, p: &[u8]) -> Result<()> {
        let mut v = (p.len() as u32).to_le_bytes().to_vec();
        v.extend_from_slice(p);
        self.send(&v)
    }
    pub fn recv_packet(&mut self) -> Result<Vec<u8>> {
        let mut l = [0u8; 4];
        self.recv_exact(&mut l)?;
        let n = u32::from_le_bytes(l) as usize;
        if n > 4096 {
            bail!("the board sent an answer of impossible size");
        }
        let mut v = vec![0u8; n];
        self.recv_exact(&mut v)?;
        Ok(v)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scalar(hex: &str) -> Scalar {
        <Scalar as Reduce<U256>>::reduce_bytes(p256::FieldBytes::from_slice(&unhex(hex)))
    }
    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    /// RFC 9382 appendix B, first vector (A = "server", B = "client").
    #[test]
    fn rfc9382_vector() {
        let w = scalar("2ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f");
        let x = scalar("43dd0fd7215bdcb482879fca3220c6a968e66d70b1356cac18bb26c84a78d729");
        let y = scalar("dcb60106f276b02606d8ef0a328c02e4b629f84f89786af5befb0bc75b6e66be");
        let a = SideA::start_with(w, x).unwrap();
        assert!(hex(&a.pa).starts_with("04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470"));
        let (pb, kb) = side_b(w, y, b"server", b"client", &a.pa).unwrap();
        assert!(hex(&pb).starts_with("0406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676"));
        let ka = a.finish(b"server", b"client", &pb).unwrap();
        for k in [&ka, &kb] {
            assert_eq!(hex(&k.ke), "0e0672dc86f8e45565d338b0540abe69");
            assert_eq!(hex(&k.kc_a), "00c12546835755c86d8c0db7851ae86f");
            assert_eq!(hex(&k.kc_b), "a9fa3406c3b781b93d804485430ca27a");
            assert_eq!(hex(&k.confirm_a()), "58ad4aa88e0b60d5061eb6b5dd93e80d9c4f00d127c65b3b35b1b5281fee38f0");
            assert_eq!(hex(&k.confirm_b()), "d3e2e547f1ae04f2dbdbf0fc4b79f8ecff2dff314b5d32fe9fcef2fb26dc459b");
        }
    }

    #[test]
    fn wrong_code_gives_different_keys() {
        let a = SideA::start(code_scalar("123456")).unwrap();
        let (pb, kb) = side_b(code_scalar("123457"), Scalar::random(&mut rand_core()), ID_CLIENT, ID_BOARD, &a.pa).unwrap();
        let ka = a.finish(ID_CLIENT, ID_BOARD, &pb).unwrap();
        assert_ne!(ka.confirm_b(), kb.confirm_b());
        let (pb, kb) = side_b(code_scalar(" 123456\n"), Scalar::random(&mut rand_core()), ID_CLIENT, ID_BOARD, &a.pa).unwrap();
        assert_eq!(a.finish(ID_CLIENT, ID_BOARD, &pb).unwrap().confirm_b(), kb.confirm_b());
    }

    #[test]
    fn rejects_bad_points() {
        let a = SideA::start(code_scalar("1")).unwrap();
        assert!(a.finish(ID_CLIENT, ID_BOARD, &[0u8; 65]).is_err());
        assert!(a.finish(ID_CLIENT, ID_BOARD, &[4u8; 65]).is_err());
    }
}
