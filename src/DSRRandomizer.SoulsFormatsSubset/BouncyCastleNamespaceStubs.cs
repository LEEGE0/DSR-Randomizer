// The pinned SoulsFormats sources contain two stale using directives for these
// namespaces but do not call BouncyCastle. Declaring the empty namespaces lets
// the bridge subset omit that otherwise-unused runtime dependency without
// modifying the pinned third-party source tree.
namespace Org.BouncyCastle.Security
{
}

namespace Org.BouncyCastle.Crypto.Agreement.Srp
{
}
