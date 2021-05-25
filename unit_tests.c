#include <stdio.h>
#include <assert.h>
#include <sys/resource.h>
#include <inttypes.h>

#include "pasta_fp.h"
#include "pasta_fq.h"
#include "crypto.h"
#include "poseidon.h"
#include "base10.h"
#include "utils.h"
#include "sha256.h"
#include "curve_checks.h"

#define ARRAY_LEN(x) (sizeof(x)/sizeof(x[0]))

#define DEFAULT_TOKEN_ID 1
static bool _verbose;
static bool _ledger_gen;

void privkey_to_hex(char *hex, const size_t len, const Scalar priv_key) {
  uint64_t priv_words[4];
  hex[0] = '\0';

  assert(len > 2*sizeof(priv_words));
  if (len < 2*sizeof(priv_words)) {
    return;
  }

  uint8_t *p = (uint8_t *)priv_words;
  fiat_pasta_fq_from_montgomery(priv_words, priv_key);
  for (size_t i = sizeof(priv_words); i > 0; i--) {
    sprintf(&hex[2*(sizeof(priv_words) - i)], "%02x", p[i - 1]);
  }
  hex[len] = '\0';
}

bool privkey_from_hex(Scalar priv_key, const char *priv_hex) {
  size_t priv_hex_len = strnlen(priv_hex, 64);
  if (priv_hex_len != 64) {
    return false;
  }
  uint8_t priv_bytes[32];
  for (size_t i = sizeof(priv_bytes); i > 0; i--) {
    sscanf(&priv_hex[2*(i - 1)], "%02hhx", &priv_bytes[sizeof(priv_bytes) - i]);
  }

  if (priv_bytes[3] & 0xc000000000000000) {
      return false;
  }

  fiat_pasta_fq_to_montgomery(priv_key, (uint64_t *)priv_bytes);

  char priv_key_hex[65];
  privkey_to_hex(priv_key_hex, sizeof(priv_key_hex), priv_key);

  // sanity check
  int result = memcmp(priv_key_hex, priv_hex, sizeof(priv_key_hex)) == 0;
  assert(result);
  return result;
}

bool privhex_to_address(char *address, const size_t len,
                        const char *account_number, const char *priv_hex) {
  Scalar priv_key;
  if (!privkey_from_hex(priv_key, priv_hex)) {
    return false;
  }

  Keypair kp;
  scalar_copy(kp.priv, priv_key);
  generate_pubkey(&kp.pub, priv_key);

  if (!generate_address(address, len, &kp.pub)) {
    return false;
  }

  if (_verbose) {
    printf("%s => %s\n", priv_hex, address);
  }
  else if (_ledger_gen) {
    printf("    # account %s\n", account_number);
    printf("    # private key %s\n", priv_hex);
    printf("    assert(test_get_address(%s) == \"%s\")\n\n",
           account_number, address);
  }

  return true;
}

void sig_to_hex(char *hex, const size_t len, const Signature sig) {
  hex[0] = '\0';

  assert(len == 2*sizeof(Signature) + 1);
  if (len < 2*sizeof(Signature) + 1) {
    return;
  }

  uint64_t words[4];
  fiat_pasta_fp_from_montgomery(words, sig.rx);
  for (size_t i = 4; i > 0; i--) {
    sprintf(&hex[16*(4 - i)], "%016lx", htole64(words[i - 1]));
  }
  fiat_pasta_fq_from_montgomery(words, sig.s);
  for (size_t i = 4; i > 0; i--) {
    sprintf(&hex[64 + 16*(4 - i)], "%016lx", htole64(words[i - 1]));
  }
}

bool sign_transaction(char *signature, const size_t len,
                      const char *account_number,
                      const char *sender_priv_hex,
                      const char *receiver_address,
                      Currency amount,
                      Currency fee,
                      Nonce nonce,
                      GlobalSlot valid_until,
                      const char *memo,
                      bool delegation,
                      uint8_t network_id) {
  Transaction txn;

  assert(len == 2*sizeof(Signature) + 1);
  if (len != 2*sizeof(Signature) + 1) {
    return false;
  }

  prepare_memo(txn.memo, memo);

  Scalar priv_key;
  if (!privkey_from_hex(priv_key, sender_priv_hex)) {
    return false;
  }

  Keypair kp;
  scalar_copy(kp.priv, priv_key);
  generate_pubkey(&kp.pub, priv_key);

  char source_str[MINA_ADDRESS_LEN];
  if (!generate_address(source_str, sizeof(source_str), &kp.pub)) {
    return false;
  }

  char *fee_payer_str = source_str;

  txn.fee = fee;
  txn.fee_token = DEFAULT_TOKEN_ID;
  read_public_key_compressed(&txn.fee_payer_pk, fee_payer_str);
  txn.nonce = nonce;
  txn.valid_until = valid_until;

  if (delegation) {
    txn.tag[0] = 0;
    txn.tag[1] = 0;
    txn.tag[2] = 1;
  }
  else {
    txn.tag[0] = 0;
    txn.tag[1] = 0;
    txn.tag[2] = 0;
  }

  read_public_key_compressed(&txn.source_pk, source_str);
  read_public_key_compressed(&txn.receiver_pk, receiver_address);
  txn.token_id = DEFAULT_TOKEN_ID;
  txn.amount = amount;
  txn.token_locked = false;

  Compressed pub_compressed;
  compress(&pub_compressed, &kp.pub);

  Signature sig;
  sign(&sig, &kp, &txn, network_id);

  if (!verify(&sig, &pub_compressed, &txn, network_id)) {
    return false;
  }

  sig_to_hex(signature, len, sig);

  if (_verbose) {
    fprintf(stderr, "%d %s\n", delegation, signature);
  }
  else if (_ledger_gen) {
    printf("    # account %s\n", account_number);
    printf("    # private key %s\n", sender_priv_hex);
    printf("    # sig=%s\n", signature);
    printf("    assert(test_sign_tx(mina.%s,\n"
           "                        %s,\n"
           "                        \"%s\",\n"
           "                        \"%s\",\n"
           "                        %zu,\n"
           "                        %zu,\n"
           "                        %u,\n"
           "                        %u,\n"
           "                        \"%s\",\n"
           "                        mina.%s) == \"%s\")\n\n",
           delegation ? "TX_TYPE_DELEGATION" : "TX_TYPE_PAYMENT",
           account_number,
           source_str,
           receiver_address,
           amount,
           fee,
           nonce,
           valid_until,
           memo,
           network_id == MAINNET_ID ? "MAINNET_ID" : "TESTNET_ID",
           signature);
  }

  return true;
}

bool check_get_address(const char *account_number,
                       const char *priv_hex, const char *address) {
  char target[MINA_ADDRESS_LEN];
  if (!privhex_to_address(target, sizeof(target), account_number, priv_hex)) {
    return false;
  }

  return strcmp(address, target) == 0;
}

bool check_sign_tx(const char *account_number,
                   const char *sender_priv_hex,
                   const char *receiver_address,
                   Currency amount,
                   Currency fee,
                   Nonce nonce,
                   GlobalSlot valid_until,
                   const char *memo,
                   bool delegation,
                   const char *signature,
                   uint8_t network_id) {
  char target[129];
  if (!sign_transaction(target, sizeof(target),
                        account_number,
                        sender_priv_hex,
                        receiver_address,
                        amount,
                        fee,
                        nonce,
                        valid_until,
                        memo,
                        delegation,
                        network_id)) {
    return false;
   }

   return strcmp(signature, target) == 0;
}

void print_scalar_as_cstruct(const Scalar x) {
  printf("        { ");
  for (size_t i = 0; i < sizeof(Scalar)/sizeof(x[0]); i++) {
    printf("0x%016lx, ", x[i]);
  }
  printf("},\n");
}

void print_affine_as_cstruct(const Affine *a) {
  printf("        {\n");
  printf("            { ");
  for (size_t i = 0; i < sizeof(Field)/sizeof(a->x[0]); i++) {
    printf("0x%016lx, ", a->x[i]);
  }
  printf(" },\n");
  printf("            { ");
  for (size_t i = 0; i < sizeof(Field)/sizeof(a->y[0]); i++) {
    printf("0x%016lx, ", a->y[i]);
  }
  printf(" },");
  printf("\n        },\n");
}

void print_scalar_as_ledger_cstruct(const Scalar x) {
  uint64_t tmp[4];
  uint8_t *p = (uint8_t *)tmp;

  fiat_pasta_fq_from_montgomery(tmp, x);
  printf("        {");
  for (size_t i = sizeof(Scalar); i > 0; i--) {
    if (i % 8 == 0) {
      printf("\n            ");
    }
    printf("0x%02x, ", p[i - 1]);
  }
  printf("\n        },\n");
}

void print_affine_as_ledger_cstruct(const Affine *a) {
  uint64_t tmp[4];
  uint8_t *p = (uint8_t *)tmp;

  fiat_pasta_fp_from_montgomery(tmp, a->x);
  printf("        {\n");
  printf("            {");
  for (size_t i = sizeof(Field); i > 0; i--) {
    if (i % 8 == 0) {
      printf("\n                ");
    }
    printf("0x%02x, ", p[i - 1]);
  }
  printf("\n            },\n");
  fiat_pasta_fp_from_montgomery(tmp, a->y);
  printf("            {");
  for (size_t i = sizeof(Field); i > 0; i--) {
    if (i % 8 == 0) {
      printf("\n                ");
    }
    printf("0x%02x, ", p[i - 1]);
  }
  printf("\n            },");
  printf("\n        },\n");
}

void generate_curve_checks(bool ledger_gen) {
  Scalar S[EPOCHS][3];
  Affine A[EPOCHS][3];

  printf("// curve_checks.h - elliptic curve unit tests\n");
  printf("//\n");
  printf("//    These constants were generated from the Mina c-reference-signer\n");

  if (ledger_gen) {
    printf("//\n");
    printf("//    Details:  https://github.com/MinaProtocol/c-reference-signer/README.markdown\n");
    printf("//    Generate: ./unit_tests ledger_gen\n");
  }

  printf("\n");
  printf("#pragma once\n");
  printf("\n");
  printf("#include \"crypto.h\"\n");

  if (!ledger_gen) {
      printf("\n");
      printf("#define THROW(x) fprintf(stderr, \"\\n!! FAILED %%s() at %%s:%%d !!\\n\\n\", \\\n");
      printf("                         __FUNCTION__, __FILE__, __LINE__); \\\n");
      printf("                 return false;\n");
      // printf("#define THROW(x) fprintf(stderr, \"FAIL %%s:%%d %%s: check failed.\\n\", __FILE__, __LINE__, __FUNCTION__); return false;");
  }

  printf("\n");
  printf("#define EPOCHS %u\n", EPOCHS);
  printf("\n");

  // Generate test scalars
  printf("// Test scalars\n");
  printf("static const Scalar S[%u][2] = {\n", EPOCHS);

  Scalar s0; // Seed with zero scalar
  explicit_bzero(s0, sizeof(s0));
  for (size_t i = 0; i < EPOCHS; i++) {
    // Generate two more scalars
    Scalar s1, s2;
    sha256_hash(s0, sizeof(s0), s1, sizeof(s1));
    scalar_from_words(s1, s1);

    sha256_hash(s1, sizeof(s1), s2, sizeof(s2));
    scalar_from_words(s2, s2);

    memcpy(S[i][0], &s0, sizeof(S[i][0]));
    memcpy(S[i][1], &s1, sizeof(S[i][1]));
    memcpy(S[i][2], &s2, sizeof(S[i][2]));

    printf("    {\n");
    if (ledger_gen) {
      print_scalar_as_ledger_cstruct(S[i][0]);
      print_scalar_as_ledger_cstruct(S[i][1]);
      // Tests do not need S2
    }
    else {
      print_scalar_as_cstruct(S[i][0]);
      print_scalar_as_cstruct(S[i][1]);
      // Tests do not need S2
    }
    printf("    },\n");

    sha256_hash(s2, sizeof(s2), s0, sizeof(s0));
    scalar_from_words(s0, s0);
    // s0 is seed for next round!
  }
  printf("};\n");
  printf("\n");

  // Generate test curve points
  printf("// Test curve points\n");
  printf("static const Affine A[%u][3] = {\n", EPOCHS);

  for (size_t i = 0; i < EPOCHS; i++) {
    // Generate three curve points
    generate_pubkey(&A[i][0], S[i][0]);
    generate_pubkey(&A[i][1], S[i][1]);
    generate_pubkey(&A[i][2], S[i][2]);

    // Check on curve
    assert(affine_is_on_curve(&A[i][0]));
    assert(affine_is_on_curve(&A[i][1]));
    assert(affine_is_on_curve(&A[i][2]));

    printf("    {\n");
    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&A[i][0]);
      print_affine_as_ledger_cstruct(&A[i][1]);
      print_affine_as_ledger_cstruct(&A[i][2]);
    }
    else {
      print_affine_as_cstruct(&A[i][0]);
      print_affine_as_cstruct(&A[i][1]);
      print_affine_as_cstruct(&A[i][2]);
    }
    printf("    },\n");
  }
  printf("};\n");
  printf("\n");

  // Generate target outputs
  printf("// Target outputs\n");
  printf("static const Affine T[%u][5] = {\n", EPOCHS);
  for (size_t i = 0; i < EPOCHS; i++) {
    Affine a3;
    Affine a4;
    union {
      // Fit in stackspace!
      Affine a5;
      Scalar s2;
    } u;

    // Test1: On curve after scaling
    assert(affine_is_on_curve(&A[i][0]));
    assert(affine_is_on_curve(&A[i][1]));
    assert(affine_is_on_curve(&A[i][2]));

    // Test2: Addition is commutative
    //     A0 + A1 == A1 + A0
    affine_add(&a3, &A[i][0], &A[i][1]); // a3 = A0 + A1
    affine_add(&a4, &A[i][1], &A[i][0]); // a4 = A1 + A0
    assert(affine_eq(&a3, &a4));
    assert(affine_is_on_curve(&a3));

    printf("    {\n");
    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&a3);
    }
    else {
      print_affine_as_cstruct(&a3);
    }

    // Test3: Scaling commutes with adding scalars
    //     G*(S0 + S1) == G*S0 + G*S1
    scalar_add(u.s2, S[i][0], S[i][1]);
    generate_pubkey(&a3, u.s2);          // a3 = G*(S0 + S1)
    affine_add(&a4, &A[i][0], &A[i][1]); // a4 = G*S0 + G*S1
    assert(affine_eq(&a3, &a4));
    assert(affine_is_on_curve(&a3));

    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&a3);
    }
    else {
      print_affine_as_cstruct(&a3);
    }

    // Test4: Scaling commutes with multiplying scalars
    //    G*(S0*S1) == S0*(G*S1)
    scalar_mul(u.s2, S[i][0], S[i][1]);
    generate_pubkey(&a3, u.s2);                // a3 = G*(S0*S1)
    affine_scalar_mul(&a4, S[i][0], &A[i][1]); // a4 = S0*(G*S1)
    assert(affine_eq(&a3, &a4));
    assert(affine_is_on_curve(&a3));

    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&a3);
    }
    else {
      print_affine_as_cstruct(&a3);
    }

    // Test5: Scaling commutes with negation
    //    G*(-S0) == -(G*S0)
    scalar_negate(u.s2, S[i][0]);
    generate_pubkey(&a3, u.s2);   // a3 = G*(-S0)
    affine_negate(&a4, &A[i][0]); // a4 = -(G*S0)
    assert(affine_eq(&a3, &a4));
    assert(affine_is_on_curve(&a3));

    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&a3);
    }
    else {
      print_affine_as_cstruct(&a3);
    }

    // Test6: Addition is associative
    //     (A0 + A1) + A2 == A0 + (A1 + A2)
    affine_add(&a3, &A[i][0], &A[i][1]);
    affine_add(&a4, &a3, &A[i][2]);      // a4 = (A0 + A1) + A2
    affine_add(&a3, &A[i][1], &A[i][2]);
    affine_add(&u.a5, &A[i][0], &a3);    // a5 = A0 + (A1 + A2)
    assert(affine_eq(&a4, &u.a5));
    assert(affine_is_on_curve(&a4));

    if (ledger_gen) {
      print_affine_as_ledger_cstruct(&a4);
    }
    else {
      print_affine_as_cstruct(&a4);
    }
    printf("    },\n");
  }
  printf("};\n\n");
  printf("bool curve_checks(void);\n\n");

  if (ledger_gen) {
     printf("\n");
     printf("** Copy the above constants and curve_checks.c into the ledger project\n");
     printf("\n");
  }
}

#define ARRAY_SAFE(...) __VA_ARGS__
#define ASSERT_POSEIDON_EQ(type, inputs, output) { \
  PoseidonCtx ctx; \
  assert(poseidon_init(&ctx, type, NULLNET_ID)); \
  Field elements[] = inputs; \
  for (size_t i = 0; i < ARRAY_LEN(elements); i++) { \
    fiat_pasta_fp_to_montgomery(elements[i], elements[i]); \
  } \
  poseidon_update(&ctx, elements, ARRAY_LEN(elements)); \
  \
  Scalar out; \
  poseidon_digest(out, &ctx); \
  \
  Scalar target = output; \
  fiat_pasta_fq_to_montgomery(target, target); \
  if (memcmp(out, target, sizeof(out)) != 0) { \
      Scalar tmp; \
      fiat_pasta_fq_from_montgomery(tmp, out); \
      fprintf(stderr, " output: {"); \
      for (size_t i = 0; i < ARRAY_LEN(out); i++) { \
          fprintf(stderr, "%luLLU", tmp[i]); \
          if (i != ARRAY_LEN(out) - 1) { \
              fprintf(stderr, ", "); \
          } \
      } \
      fprintf(stderr, "}\n"); \
      fiat_pasta_fq_from_montgomery(tmp, target); \
      fprintf(stderr, " target: {"); \
      for (size_t i = 0; i < ARRAY_LEN(target); i++) { \
          fprintf(stderr, "%luLLU", tmp[i]); \
          if (i != ARRAY_LEN(target) - 1) { \
              fprintf(stderr, ", "); \
          } \
      } \
      fprintf(stderr, "}\n"); \
      assert(memcmp(out, target, sizeof(out)) == 0); \
  } \
}

int main(int argc, char* argv[]) {
  printf("Running unit tests\n");

  if (argc > 1) {
    if (strncmp(argv[1], "ledger_gen", 10) == 0) {
        _ledger_gen = true;
    }
    else {
        _verbose = true;
    }
  }
  struct rlimit lim = {1, 1};
  if (setrlimit(RLIMIT_STACK, &lim) == -1) {
    printf("rlimit failed\n");
    return 1;
  }

  // Address tests

  if (_ledger_gen) {
    printf("    # Address generation tests\n");
    printf("    #\n");
    printf("    #     These tests were automatically generated from the Mina c-reference-signer\n");
    printf("    #\n");
    printf("    #     Details:  https://github.com/MinaProtocol/c-reference-signer/README.markdown\n");
    printf("    #     Generate: ./unit_tests ledger_gen\n");
    printf("\n");
  }

  assert(check_get_address("0",
                           "164244176fddb5d769b7de2027469d027ad428fadcc0c02396e6280142efb718",
                           "B62qnzbXmRNo9q32n4SNu2mpB8e7FYYLH8NmaX6oFCBYjjQ8SbD7uzV"));

  assert(check_get_address("1",
                           "3ca187a58f09da346844964310c7e0dd948a9105702b716f4d732e042e0c172e",
                           "B62qicipYxyEHu7QjUqS7QvBipTs5CzgkYZZZkPoKVYBu6tnDUcE9Zt"));

  assert(check_get_address("2",
                           "336eb4a19b3d8905824b0f2254fb495573be302c17582748bf7e101965aa4774",
                           "B62qrKG4Z8hnzZqp1AL8WsQhQYah3quN1qUj3SyfJA8Lw135qWWg1mi"));

  assert(check_get_address("3",
                           "1dee867358d4000f1dafa5978341fb515f89eeddbe450bd57df091f1e63d4444",
                           "B62qoqiAgERjCjXhofXiD7cMLJSKD8hE8ZtMh4jX5MPNgKB4CFxxm1N"));

  assert(check_get_address("49370",
                           "20f84123a26e58dd32b0ea3c80381f35cd01bc22a20346cc65b0a67ae48532ba",
                           "B62qkiT4kgCawkSEF84ga5kP9QnhmTJEYzcfgGuk6okAJtSBfVcjm1M"));

  assert(check_get_address("0x312a",
                           "3414fc16e86e6ac272fda03cf8dcb4d7d47af91b4b726494dab43bf773ce1779",
                           "B62qoG5Yk4iVxpyczUrBNpwtx2xunhL48dydN53A2VjoRwF8NUTbVr4"));

  // Sign payment tx tests

  if (_ledger_gen) {
    printf("    # Sign transaction tests\n");
    printf("    #\n");
    printf("    #     These tests were automatically generated from the Mina c-reference-signer\n");
    printf("    #\n");
    printf("    #     Details:  https://github.com/MinaProtocol/c-reference-signer/README.markdown\n");
    printf("    #     Generate: ./unit_tests ledger_gen\n");
    printf("\n");
  }

  uint8_t network_ids[2] = { TESTNET_ID, MAINNET_ID };

  char* signatures[2][8] = {
    {
      "11a36a8dfe5b857b95a2a7b7b17c62c3ea33411ae6f4eb3a907064aecae353c60794f1d0288322fe3f8bb69d6fabd4fd7c15f8d09f8783b2f087a80407e299af",
      "23a9e2375dd3d0cd061e05c33361e0ba270bf689c4945262abdcc81d7083d8c311ae46b8bebfc98c584e2fb54566851919b58cf0917a256d2c1113daa1ccb27f",
      "2b4d0bffcb57981d11a93c05b17672b7be700d42af8496e1ba344394da5d0b0b0432c1e8a77ee1bd4b8ef6449297f7ed4956b81df95bdc6ac95d128984f77205",
      "25bb730a25ce7180b1e5766ff8cc67452631ee46e2d255bccab8662e5f1f0c850a4bb90b3e7399e935fff7f1a06195c6ef89891c0260331b9f381a13e5507a4c",
      "30797d7d0426e54ff195d1f94dc412300f900cc9e84990603939a77b3a4d2fc11ebab12857b47c481c182abe147279732549f0fd49e68d5541f825e9d1e6fa04",
      "07e9f88fc671ed06781f9edb233fdbdee20fa32303015e795747ad9e43fcb47b3ce34e27e31f7c667756403df3eb4ce670d9175dd0ae8490b273485b71c56066",
      "1ff9f77fed4711e0ebe2a7a46a7b1988d1b62a850774bf299ec71a24d5ebfdd81d04a570e4811efe867adefe3491ba8b210f24bd0ec8577df72212d61b569b15",
      "26ca6b95dee29d956b813afa642a6a62cd89b1929320ed6b099fd191a217b08d2c9a54ba1c95e5000b44b93cfbd3b625e20e95636f1929311473c10858a27f09"
    },
    {
      "124c592178ed380cdffb11a9f8e1521bf940e39c13f37ba4c55bb4454ea69fba3c3595a55b06dac86261bb8ab97126bf3f7fff70270300cb97ff41401a5ef789",
      "204eb1a37e56d0255921edd5a7903c210730b289a622d45ed63a52d9e3e461d13dfcf301da98e218563893e6b30fa327600c5ff0788108652a06b970823a4124",
      "076d8ebca8ccbfd9c8297a768f756ff9d08c049e585c12c636d57ffcee7f6b3b1bd4b9bd42cc2cbee34b329adbfc5127fe5a2ceea45b7f55a1048b7f1a9f7559",
      "058ed7fb4e17d9d400acca06fe20ca8efca2af4ac9a3ed279911b0bf93c45eea0e8961519b703c2fd0e431061d8997cac4a7574e622c0675227d27ce2ff357d9",
      "0904e9521a95334e3f6757cb0007ec8af3322421954255e8d263d0616910b04d213344f8ec020a4b873747d1cbb07296510315a2ec76e52150a4c765520d387f",
      "2406ab43f8201bd32bdd81b361fdb7871979c0eec4e3b7a91edf87473963c8a4069f4811ebc5a0e85cbb4951bffe93b638e230ce5a250cb08d2c250113a1967c",
      "36a80d0421b9c0cbfa08ea95b27f401df108b30213ae138f1f5978ffc59606cf2b64758db9d26bd9c5b908423338f7445c8f0a07520f2154bbb62926aa0cb8fa",
      "093f9ef0e4e051279da0a3ded85553847590ab739ee1bfd59e5bb30f98ed8a001a7a60d8506e2572164b7a525617a09f17e1756ac37555b72e01b90f37271595",
    }
  };

  for (size_t i = 0; i < 2; ++i) {
    uint8_t network_id = network_ids[i];
    assert(check_sign_tx("0",
                        "164244176fddb5d769b7de2027469d027ad428fadcc0c02396e6280142efb718",
                        "B62qicipYxyEHu7QjUqS7QvBipTs5CzgkYZZZkPoKVYBu6tnDUcE9Zt",
                        1729000000000,
                        2000000000,
                        16,
                        271828,
                        "Hello Mina!",
                        false,
                        signatures[i][0],
                        network_id));

    assert(check_sign_tx("12586",
                        "3414fc16e86e6ac272fda03cf8dcb4d7d47af91b4b726494dab43bf773ce1779",
                        "B62qrKG4Z8hnzZqp1AL8WsQhQYah3quN1qUj3SyfJA8Lw135qWWg1mi",
                        314159265359,
                        1618033988,
                        0,
                        4294967295,
                        "",
                        false,
                        signatures[i][1],
                        network_id));

    assert(check_sign_tx("12586",
                        "3414fc16e86e6ac272fda03cf8dcb4d7d47af91b4b726494dab43bf773ce1779",
                        "B62qoqiAgERjCjXhofXiD7cMLJSKD8hE8ZtMh4jX5MPNgKB4CFxxm1N",
                        271828182845904,
                        100000,
                        5687,
                        4294967295,
                        "01234567890123456789012345678901",
                        false,
                        signatures[i][2],
                        network_id));

    assert(check_sign_tx("3",
                        "1dee867358d4000f1dafa5978341fb515f89eeddbe450bd57df091f1e63d4444",
                        "B62qnzbXmRNo9q32n4SNu2mpB8e7FYYLH8NmaX6oFCBYjjQ8SbD7uzV",
                        0,
                        2000000000,
                        0,
                        1982,
                        "",
                        false,
                        signatures[i][3],
                        network_id));

    // Sign delegation tx tests

    assert(check_sign_tx("0",
                        "164244176fddb5d769b7de2027469d027ad428fadcc0c02396e6280142efb718",
                        "B62qicipYxyEHu7QjUqS7QvBipTs5CzgkYZZZkPoKVYBu6tnDUcE9Zt",
                        0,
                        2000000000,
                        16,
                        1337,
                        "Delewho?",
                        true,
                        signatures[i][4],
                        network_id));

    assert(check_sign_tx("49370",
                        "20f84123a26e58dd32b0ea3c80381f35cd01bc22a20346cc65b0a67ae48532ba",
                        "B62qnzbXmRNo9q32n4SNu2mpB8e7FYYLH8NmaX6oFCBYjjQ8SbD7uzV",
                        0,
                        2000000000,
                        0,
                        4294967295,
                        "",
                        true,
                        signatures[i][5],
                        network_id));

    assert(check_sign_tx("12586",
                        "3414fc16e86e6ac272fda03cf8dcb4d7d47af91b4b726494dab43bf773ce1779",
                        "B62qkiT4kgCawkSEF84ga5kP9QnhmTJEYzcfgGuk6okAJtSBfVcjm1M",
                        0,
                        42000000000,
                        1,
                        4294967295,
                        "more delegates, more fun........",
                        true,
                        signatures[i][6],
                        network_id));

    assert(check_sign_tx("2",
                        "336eb4a19b3d8905824b0f2254fb495573be302c17582748bf7e101965aa4774",
                        "B62qicipYxyEHu7QjUqS7QvBipTs5CzgkYZZZkPoKVYBu6tnDUcE9Zt",
                        0,
                        1202056900,
                        0,
                        577216,
                        "",
                        true,
                        signatures[i][7],
                        network_id));
  }

  // Check testnet and mainnet signatures are not equal
  for (size_t i = 0; i < 8; ++i) {
      assert(strncmp(signatures[0][i], signatures[1][i], strlen(signatures[1][i])) != 0);
  }

  // 3-wire poseidon tests

  ASSERT_POSEIDON_EQ(
    POSEIDON_3W,
    ARRAY_SAFE({
    }),
    ARRAY_SAFE(
      {17114291637813588507ULL, 14335107542818720711ULL, 1320934316380316157ULL, 1722173086297925183ULL}
     )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3W,
    ARRAY_SAFE({
      {11416295947058400506ULL, 3360729831846485862ULL, 12146560982654972456ULL, 2987985415332862884ULL}
    }),
    ARRAY_SAFE(
      {871590621865441384ULL, 15942464099191336363ULL, 2836661416333151733ULL, 11819778491522761ULL}
     )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3W,
    ARRAY_SAFE({
      {16049149342757733248ULL, 17845879034270049224ULL, 6274988087599189421ULL, 3891307270444217155ULL},
      {9941995706707671113ULL, 236362462947459140ULL, 17033003259035381397ULL, 4098833191871625741ULL}
     }),
    ARRAY_SAFE(
      {17256859529285183666ULL, 10562454737368249340ULL, 16653501986100235558ULL, 1613229473904780795ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3W,
    ARRAY_SAFE({
      {16802949773563312590ULL, 13786671686687654025ULL, 6327949131269833714ULL, 2206832697832183571ULL},
      {18422989176992908572ULL, 7121908340714489421ULL, 15983151711675082713ULL, 2047309793776126211ULL},
      {10656504003679202293ULL, 5033073342697291414ULL, 15641563258223497348ULL, 2549024716872047224ULL}
    }),
    ARRAY_SAFE(
      {4610990272905062813ULL, 1786831480172390544ULL, 12827185513759772316ULL, 1463055697820942106ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3W,
    ARRAY_SAFE({
      {13568896335663078044ULL, 12780551435489493364ULL, 7939944734757335782ULL, 2716817606766379733ULL},
      {8340509593943796912ULL, 14326728421072412984ULL, 1939214290157533341ULL, 248823904156563876ULL},
      {18138459445226614284ULL, 7569000930215382240ULL, 12226032416704596818ULL, 754852930030810284ULL},
      {11813108562003481058ULL, 3775716673546104688ULL, 7004407702567408918ULL, 2198318152235466722ULL},
      {9752122577441799495ULL, 2743141496725547769ULL, 8526535807986851558ULL, 1154473298561249145ULL},
      {12335717698867852470ULL, 17616685850532508842ULL, 8342889821739786893ULL, 2726231867163795098ULL}
    }),
    ARRAY_SAFE(
      {2534358780431475408ULL, 3747832072933808141ULL, 2500060454948506474ULL, 2342403740596596240ULL}
    )
  );

  // 5-wire poseidon tests

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
    }),
    ARRAY_SAFE(
      {11864518339837020673ULL, 11154701827270369066ULL, 18250329647482904211ULL, 2973895537517503096ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
      {925605326051629702ULL, 9450022185177868805ULL, 3430781963795317176ULL, 2120098912251973017ULL}
    }),
    ARRAY_SAFE(
      {2462689009389580473ULL, 17870513234387686250ULL, 11236274956264243810ULL, 3641294289935218438ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
      {4872213112846934187ULL, 15221974649365942201ULL, 4177652558587823268ULL, 1324361518338458527ULL},
      {10368205141323064185ULL, 9471328583611422132ULL, 12997197966961952901ULL, 3290733940621514661ULL}
    }),
    ARRAY_SAFE(
      {6903622620367681812ULL, 11040552022054417145ULL, 756305575883948511ULL, 2025491032262703105ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
      {7832849012654337787ULL, 4963068119957452774ULL, 10773086124514989319ULL, 1683727612549340848ULL},
      {3569008656860171438ULL, 10394421784622027030ULL, 196192141273432503ULL, 1248957759478765405ULL},
      {9522737303355578738ULL, 572132462899615385ULL, 13566429773365192181ULL, 121306779591653499ULL},
      {13250259935835462717ULL, 4425586510556471497ULL, 14507184955230611679ULL, 2566418502016358110ULL}
    }),
    ARRAY_SAFE(
      {15890326985419680819ULL, 13328868938658098350ULL, 14092994142147217030ULL, 1596359391679724262ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
      {17910451947845015148ULL, 5322223719857525348ULL, 10480894361828395044ULL, 34781755494926625ULL},
      {6570939701805895370ULL, 4169423915667089544ULL, 2366634926126932666ULL, 1804659639444390640ULL},
      {13670464873640336259ULL, 14938327700162099274ULL, 9664883370546456952ULL, 2153565343801502671ULL},
      {6187547161975656466ULL, 12648383547735143102ULL, 15485540615689340699ULL, 417108511095786061ULL},
      {3554897497035940734ULL, 1047125997069612643ULL, 8351564331993121170ULL, 2878650169515721164ULL}
    }),
    ARRAY_SAFE(
      {4479424786655393812ULL, 790574497228972985ULL, 13640155489552216446ULL, 711750288597225015ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_5W,
    ARRAY_SAFE({
      {13179872908007675812ULL, 15426428840987667748ULL, 15925112389472812618ULL, 1172338616269137102ULL},
      {9811926356385353149ULL, 16140323422473131507ULL, 1062272508702625050ULL, 1217048734747816216ULL},
      {9487959623437049412ULL, 8184175053892911879ULL, 12241988285373791715ULL, 528401480102984021ULL},
      {2797989853748670076ULL, 10357979140364496699ULL, 12883675067488813586ULL, 2675529708005952482ULL},
      {8051500605615959931ULL, 13944994468851713843ULL, 9308072337342366951ULL, 3594361030023669619ULL},
      {6680331634300327182ULL, 6761417420987938685ULL, 10683832798558320757ULL, 2470756527121432589ULL}
    }),
    ARRAY_SAFE(
      {3614205655220390000ULL, 4108372806675450262ULL, 3652960650983359474ULL, 2116997592584139383ULL}
    )
  );

  // poseidon3 tests

  ASSERT_POSEIDON_EQ(
    POSEIDON_3,
    ARRAY_SAFE({
    }),
    ARRAY_SAFE(
      {12625032309730357895ULL, 3881775963142723428ULL, 1948451027071626224ULL, 400220142328418896ULL}
     )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3,
    ARRAY_SAFE({
      {7268460211608788188ULL, 10132480989041334579ULL, 2339874299280274918ULL, 194293202993774285ULL}
    }),
    ARRAY_SAFE(
      {13319422177750956895ULL, 2619256142820001370ULL, 15974443115283230879ULL, 4444502174303366803ULL}
     )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3,
    ARRAY_SAFE({
      {9917828826452988051ULL, 15189182483242825728ULL, 17783867389905310625ULL, 3096233339466922731ULL},
      {11112469648615694507ULL, 1349483555912170531ULL, 5132274865255624365ULL, 291635065414725798ULL}
     }),
    ARRAY_SAFE(
      {8899203031135722773ULL, 16362655810520084016ULL, 4138942464075294076ULL, 3369640260295132563ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3,
    ARRAY_SAFE({
      {14267996300018486948ULL, 670373130142722849ULL, 4216114176990048262ULL, 3881970950122376215ULL},
      {2734205406253254786ULL, 17095706724646389267ULL, 5933659775356387652ULL, 3721674824441362406ULL},
      {4947525329177827161ULL, 2645489287737017668ULL, 9857560748408218200ULL, 1227757243736002830ULL}
    }),
    ARRAY_SAFE(
      {3994882120963907648ULL, 16872604210008740744ULL, 16804570819156007307ULL, 1861001323535343521ULL}
    )
  );

  ASSERT_POSEIDON_EQ(
    POSEIDON_3,
    ARRAY_SAFE({
      {7267853995951905224ULL, 90403176695802388ULL, 4774599761789790556ULL, 3347377905747449096ULL},
      {11838594320814769562ULL, 278541806768709143ULL, 4632615733560524785ULL, 2328922649099910504ULL},
      {17911298769116557437ULL, 6834069749734115640ULL, 9177656000002681079ULL, 2795336499778575742ULL},
      {7151979636429903658ULL, 14400997240730962670ULL, 4625828803120157807ULL, 1840002810696946942ULL},
      {10973288036385879140ULL, 15163372292438207457ULL, 8171725748546728133ULL, 4039739380933749593ULL},
      {14659358909991100974ULL, 4969649262916868094ULL, 16870234378475169070ULL, 2694211618115933100ULL}
    }),
    ARRAY_SAFE(
      {5634255577245254270ULL, 14395092878371292826ULL, 16978463518186927900ULL, 178730575833426237ULL}
    )
  );

  // Perform crypto tests
  if (!curve_checks()) {
      // Dump computed c-reference signer constants
      generate_curve_checks(false);
      fprintf(stderr, "!! Curve checks FAILED !! (error above)\n\n");
      exit(211);
  }
  if (_ledger_gen) {
      generate_curve_checks(true);
  }

  printf("Unit tests completed successfully\n");

  return 0;
}
