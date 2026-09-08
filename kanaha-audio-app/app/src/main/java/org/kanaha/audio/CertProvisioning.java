/*
 * Kanaha Audio — on-device certificate provisioning (milestone B, RAPI-style).
 *
 * The device mints its OWN RSA keypair and PKCS#10 CSR on first run; the private
 * key never leaves the device. An operator pulls files/csr/calcs.csr, signs it
 * with the off-device Kanaha CA (see ~/kanaha-ca/kanaha-provision.sh), and pushes
 * back a CA-signed server.crt + the CA cert. The httpd only starts once that
 * provisioning has happened (strict "key never travels" / provision-required model).
 */
package org.kanaha.audio;

import android.os.Build;
import android.util.Log;

import org.bouncycastle.jce.provider.BouncyCastleProvider;
import org.bouncycastle.openssl.jcajce.JcaPEMWriter;
import org.bouncycastle.operator.ContentSigner;
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder;
import org.bouncycastle.pkcs.PKCS10CertificationRequest;
import org.bouncycastle.pkcs.jcajce.JcaPKCS10CertificationRequestBuilder;
import org.bouncycastle.util.io.pem.PemObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileWriter;
import java.io.InputStream;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.Security;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;

import javax.security.auth.x500.X500Principal;

public class CertProvisioning {
    private static final String TAG = "KanahaAudioProvision";

    static {
        // Android ships a stripped "BC" provider; replace it with the full one so
        // bcpkix (PKCS#10 CSR building) works.
        Security.removeProvider(BouncyCastleProvider.PROVIDER_NAME);
        Security.insertProviderAt(new BouncyCastleProvider(), 1);
    }

    private final File sslDir;
    private final File csrDir;

    public CertProvisioning(File filesDir) {
        this.sslDir = new File(filesDir, "apache/ssl");
        this.csrDir = new File(filesDir, "csr");
    }

    /** Device identity used as the cert CN (DNS-safe Build.MODEL). */
    public static String deviceCn() {
        String base = Build.MODEL != null ? Build.MODEL : "device";
        String safe = base.replaceAll("[^A-Za-z0-9-]", "-").replaceAll("-+", "-");
        while (safe.startsWith("-")) safe = safe.substring(1);
        while (safe.endsWith("-")) safe = safe.substring(0, safe.length() - 1);
        return safe.isEmpty() ? "device" : safe;
    }

    /**
     * Generate the RSA-2048 keypair and PKCS#10 CSR on first run if absent. The
     * private key (server.key) never leaves the device; the CSR (csr/calcs.csr)
     * is what the operator pulls and signs with the Kanaha CA.
     */
    public File ensureKeypairAndCsr() throws Exception {
        sslDir.mkdirs();
        csrDir.mkdirs();
        File keyFile = new File(sslDir, "server.key");
        File csrFile = new File(csrDir, "audio.csr");
        if (keyFile.exists() && keyFile.length() > 0 && csrFile.exists() && csrFile.length() > 0) {
            return csrFile;
        }
        String cn = deviceCn();
        Log.i(TAG, "Generating device keypair + CSR (CN=" + cn + ")");

        KeyPairGenerator kpg = KeyPairGenerator.getInstance("RSA", BouncyCastleProvider.PROVIDER_NAME);
        kpg.initialize(2048);
        KeyPair kp = kpg.generateKeyPair();

        // getEncoded() is PKCS#8 DER; wrap as a "PRIVATE KEY" PEM (not BC's default
        // PKCS#1 "RSA PRIVATE KEY") so Axis2Client's PKCS8 parser and httpd both read it.
        writePem(keyFile, new PemObject("PRIVATE KEY", kp.getPrivate().getEncoded()));
        // App-private key: owner read/write only.
        keyFile.setReadable(false, false);
        keyFile.setReadable(true, true);
        keyFile.setWritable(false, false);
        keyFile.setWritable(true, true);

        X500Principal subject = new X500Principal("O=Kanaha, OU=kanaha-audio, CN=" + cn);
        JcaPKCS10CertificationRequestBuilder p10 =
            new JcaPKCS10CertificationRequestBuilder(subject, kp.getPublic());
        ContentSigner signer = new JcaContentSignerBuilder("SHA256WithRSA")
            .setProvider(BouncyCastleProvider.PROVIDER_NAME)
            .build(kp.getPrivate());
        PKCS10CertificationRequest csr = p10.build(signer);
        writePem(csrFile, csr);

        Log.i(TAG, "CSR written: " + csrFile.getAbsolutePath()
                + " — awaiting provisioning (adb pull + sign with the Kanaha CA)");
        return csrFile;
    }

    /**
     * True once a CA-signed server.crt and the CA cert are present. A self-signed
     * cert (issuer == subject) does not count — only a real CA-signed leaf does.
     */
    public boolean isProvisioned() {
        File crt = new File(sslDir, "server.crt");
        File ca = new File(sslDir, "ca.crt");
        if (!crt.exists() || !ca.exists() || crt.length() == 0 || ca.length() == 0) {
            return false;
        }
        try {
            X509Certificate c = readCert(crt);
            return !c.getIssuerX500Principal().equals(c.getSubjectX500Principal());
        } catch (Exception e) {
            Log.w(TAG, "server.crt unreadable: " + e.getMessage());
            return false;
        }
    }

    private static void writePem(File f, Object obj) throws Exception {
        try (JcaPEMWriter w = new JcaPEMWriter(new FileWriter(f))) {
            w.writeObject(obj);
        }
    }

    private static X509Certificate readCert(File f) throws Exception {
        try (InputStream in = new FileInputStream(f)) {
            return (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(in);
        }
    }
}
