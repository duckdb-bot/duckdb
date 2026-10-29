#include "catch.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "test_helpers.hpp"

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <sys/resource.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#endif

using namespace duckdb;

#if defined(__linux__) && !defined(DUCKDB_NO_THREADS)

namespace {

// An RSA 2048 key pair made for this test with openssl genrsa. This is the public key in the compact form that
// CREATE EXTENSION REPOSITORY takes (base64 of the DER encoding). The private key only signed SIGNATURE_HEX below.
constexpr const char *SIGNING_PUBLIC_KEY =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA4TQiDZgqbR0dQu/x9vqx+kxCQ5jJxYf75y/aHAffMGcn0W1rfwkbX+48SJu3wepD4Q"
    "hBg02XYaNgnlqeMa9eYHkxo4tQ09g36c1cMNuVLvb75fG0MrZMP0GU086ydp/UbjLcRMZHUuLE8PMCOFUqrwMlt/qA82I2naNpS9Efjid/btzE"
    "F/gtWUY8XyLAKRcjCupdzIaxBhwOQkeAZJrP+r9A8FdoA0TnUCauiUW4qkNq0qe2wp41R48yFuBJlX0+miJ9QaNorYkmv7kpknypZ+oEeLd3Ql"
    "KPwqCCDPYc7OYkAncoOu9QIHVGC66urNqKTCMOUK8BZY8CBEAbeyUUdwIDAQAB";

// The extension buffer of the test is BUFFER_SIZE bytes, all zero except for the signature in its last 256 bytes.
// The signed part is hashed in four 1 MiB chunks (the last one 256 bytes short), so four hashing threads start.
constexpr idx_t BUFFER_SIZE = 4ULL * 1024 * 1024;

// The RSASSA PKCS1 v1.5 signature of the two level hash of the signed part (sha256 of the concatenated sha256 of
// each chunk), made with openssl pkeyutl in sign mode with the sha256 digest option
constexpr const char *SIGNATURE_HEX =
    "21ff30cc5141152f66604f84942ec866593738f50630dfa39811e1c6b976af93ce7cc9909b143f8011a520022a7acdd0e333b9c077c8c4"
    "3fe75eba3f3d3fdda16aabe4b6533041a32e9004a23d3babf5294f0066270c17b116f3b392bcd0c2409f57ab1d058415cf4192db20fd9f4"
    "1946873c3897852cff5d639d82aeb08ebf5901ea97badc8312f164ba0e6f496ace28a16a6630ec7d07be018818d2ec2465d8157c0d9d03"
    "260e54eea42c05e90df2bf8d1540fe009469f329d6adeea4f1de08c7715e221455baac28393c7112cdf117f70058163dc9d8ebb07b6706e"
    "553758dace8cfcb8d31c54990a5d25a63b4193c95da1ce84d11edd9daeb539eed52de7";

vector<char> SignedBuffer() {
	vector<char> buffer(BUFFER_SIZE, 0);
	const string hex(SIGNATURE_HEX);
	REQUIRE(hex.size() == 2 * ParsedExtensionMetaData::SIGNATURE_SIZE);
	auto signature = buffer.data() + BUFFER_SIZE - ParsedExtensionMetaData::SIGNATURE_SIZE;
	for (idx_t i = 0; i < ParsedExtensionMetaData::SIGNATURE_SIZE; i++) {
		signature[i] = static_cast<char>(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
	}
	return buffer;
}

//! The address space of this process in bytes
idx_t CurrentAddressSpace() {
	std::ifstream statm("/proc/self/statm");
	idx_t pages = 0;
	statm >> pages;
	return pages * NumericCast<idx_t>(sysconf(_SC_PAGESIZE));
}

//! Lets exactly two more threads start. Every new thread gets a 256 MiB stack and the address space limit leaves
//! room for two of them, so the third pthread_create fails with EAGAIN, as it does under a thread limit. The
//! destructor restores both settings.
class TwoMoreThreads {
public:
	static constexpr idx_t STACK_SIZE = 256ULL * 1024 * 1024;

	TwoMoreThreads() {
		pthread_getattr_default_np(&old_attr);
		pthread_attr_init(&new_attr);
		pthread_attr_setstacksize(&new_attr, STACK_SIZE);
		attr_set = pthread_setattr_default_np(&new_attr) == 0;
		getrlimit(RLIMIT_AS, &old_limit);
		auto new_limit = old_limit;
		new_limit.rlim_cur = CurrentAddressSpace() + STACK_SIZE * 5 / 2;
		limit_set = setrlimit(RLIMIT_AS, &new_limit) == 0;
	}
	~TwoMoreThreads() {
		if (limit_set) {
			setrlimit(RLIMIT_AS, &old_limit);
		}
		if (attr_set) {
			pthread_setattr_default_np(&old_attr);
		}
		pthread_attr_destroy(&new_attr);
		pthread_attr_destroy(&old_attr);
	}

	//! Whether two threads start and the third does not, which needs a platform that honors both settings
	bool Works() const {
		if (!attr_set || !limit_set) {
			return false;
		}
		std::mutex gate;
		gate.lock();
		vector<std::thread> threads;
		bool third_failed = false;
		try {
			for (idx_t i = 0; i < 3; i++) {
				threads.emplace_back([&gate]() { std::lock_guard<std::mutex> hold(gate); });
			}
		} catch (std::system_error &) {
			third_failed = threads.size() == 2;
		}
		gate.unlock();
		for (auto &thread : threads) {
			thread.join();
		}
		return third_failed;
	}

private:
	pthread_attr_t old_attr;
	pthread_attr_t new_attr;
	bool attr_set = false;
	rlimit old_limit;
	bool limit_set = false;
};

} // namespace

TEST_CASE("Test that a signature check survives a hashing thread that cannot start", "[api]") {
	DBConfig config;
	config.SetOptionByName("extension_repository_directory", TestCreatePath("extension_signature_check"));
	config.SetOptionByName("allow_extension_repositories", "allowed");
	DuckDB db(nullptr, &config);
	Connection con(db);
	auto &instance = *db.instance;
	REQUIRE_NO_FAIL(con.Query(
	    StringUtil::Format("CREATE EXTENSION REPOSITORY signed WITH PREFIX 'http://signed.test' USING PUBLIC KEY '%s'",
	                       SIGNING_PUBLIC_KEY)));
	auto buffer = SignedBuffer();

	// the signature verifies on the usual path, which hashes the four chunks on one thread each
	REQUIRE(ExtensionHelper::CheckExtensionBufferSignature(instance, buffer.data(), BUFFER_SIZE,
	                                                       ExtensionRepositoryType::USER_PROVIDED, "signed"));

	TwoMoreThreads two_more_threads;
	if (!two_more_threads.Works()) {
		WARN("the thread limit of this test does not take effect here, so the check under it is skipped");
		return;
	}
	// the third hashing thread cannot start, so the check hashes the remaining chunks on this thread and still
	// verifies the signature, instead of terminating the process
	REQUIRE(ExtensionHelper::CheckExtensionBufferSignature(instance, buffer.data(), BUFFER_SIZE,
	                                                       ExtensionRepositoryType::USER_PROVIDED, "signed"));
}

#endif
