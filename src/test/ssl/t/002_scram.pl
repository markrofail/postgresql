# Test SCRAM authentication and TLS channel binding types

use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More;

use File::Copy;

use FindBin;
use lib $FindBin::RealBin;

use SSLServer;

if ($ENV{with_ssl} ne 'openssl')
{
	plan skip_all => 'OpenSSL not supported by this build';
}

# This is the hostname used to connect to the server.
my $SERVERHOSTADDR = '127.0.0.1';
# This is the pattern to use in pg_hba.conf to match incoming connections.
my $SERVERHOSTCIDR = '127.0.0.1/32';

# Determine whether build supports tls-server-end-point.
my $supports_tls_server_end_point =
  check_pg_config("#define HAVE_X509_GET_SIGNATURE_NID 1");

my $number_of_tests = $supports_tls_server_end_point ? 15 : 16;

# Allocation of base connection string shared among multiple tests.
my $common_connstr;

# Set up the server.

note "setting up data directory";
my $node = get_new_node('primary');
$node->init;

# PGHOST is enforced here to set up the node, subsequent connections
# will use a dedicated connection string.
$ENV{PGHOST} = $node->host;
$ENV{PGPORT} = $node->port;
$node->start;

# Configure server for SSL connections, with password handling.
configure_test_server_for_ssl($node, $SERVERHOSTADDR, $SERVERHOSTCIDR,
	"scram-sha-256", "pass", "scram-sha-256");
switch_server_cert($node, 'server-cn-only');
$common_connstr =
  "dbname=trustdb sslmode=require sslcert=invalid sslrootcert=invalid hostaddr=$SERVERHOSTADDR";

my $log = $node->rotate_logfile();
$node->restart;

# Bad password
$ENV{PGPASSWORD} = "badpass";
test_connect_fails($common_connstr, "user=ssltestuser",
	qr/password authentication failed/,
	"Basic SCRAM authentication with bad password");

$node->stop('fast');
my $log_contents = slurp_file($log);

unlike(
	$log_contents,
	qr/connection authenticated:/,
	"SCRAM does not set authenticated identity with bad password");

$log = $node->rotate_logfile();
$node->start;

# Default settings
$ENV{PGPASSWORD} = "pass";
test_connect_ok($common_connstr, "user=ssltestuser",
	"Basic SCRAM authentication with SSL");

$node->stop('fast');
$log_contents = slurp_file($log);

like(
	$log_contents,
	qr/connection authenticated: identity="ssltestuser" method=scram-sha-256/,
	"Basic SCRAM sets the username as the authenticated identity");

$node->start;

# Test channel_binding
test_connect_fails(
	$common_connstr,
	"user=ssltestuser channel_binding=invalid_value",
	qr/invalid channel_binding value: "invalid_value"/,
	"SCRAM with SSL and channel_binding=invalid_value");
test_connect_ok(
	$common_connstr,
	"user=ssltestuser channel_binding=disable",
	"SCRAM with SSL and channel_binding=disable");
if ($supports_tls_server_end_point)
{
	test_connect_ok(
		$common_connstr,
		"user=ssltestuser channel_binding=require",
		"SCRAM with SSL and channel_binding=require");
}
else
{
	test_connect_fails(
		$common_connstr,
		"user=ssltestuser channel_binding=require",
		qr/channel binding is required, but server did not offer an authentication method that supports channel binding/,
		"SCRAM with SSL and channel_binding=require");
}

# Now test when the user has an MD5-encrypted password; should fail
test_connect_fails(
	$common_connstr,
	"user=md5testuser channel_binding=require",
	qr/channel binding required but not supported by server's authentication request/,
	"MD5 with SSL and channel_binding=require");

# Now test with auth method 'cert' by connecting to 'certdb'. Should fail,
# because channel binding is not performed.  Note that ssl/client.key may
# be used in a different test, so the name of this temporary client key
# is chosen here to be unique.
my $client_tmp_key = "ssl/client_scram_tmp.key";
copy("ssl/client.key", $client_tmp_key);
chmod 0600, $client_tmp_key;
test_connect_fails(
	"sslcert=ssl/client.crt sslkey=$client_tmp_key sslrootcert=invalid hostaddr=$SERVERHOSTADDR",
	"dbname=certdb user=ssltestuser channel_binding=require",
	qr/channel binding required, but server authenticated client without channel binding/,
	"Cert authentication and channel_binding=require");

$log = $node->rotate_logfile();
$node->restart;

# Certificate verification at the connection level should still work fine.
test_connect_ok(
	"sslcert=ssl/client.crt sslkey=$client_tmp_key sslrootcert=invalid hostaddr=$SERVERHOSTADDR",
	"dbname=verifydb user=ssltestuser channel_binding=require",
	"SCRAM with clientcert=verify-full and channel_binding=require");

$node->stop('fast');
$log_contents = slurp_file($log);

like(
	$log_contents,
	qr/connection authenticated: identity="ssltestuser" method=scram-sha-256/,
	"SCRAM with clientcert=verify-full sets the username as the authenticated identity");

$node->start;

# clean up
unlink($client_tmp_key);

done_testing($number_of_tests);
