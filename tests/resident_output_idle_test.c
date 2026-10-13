#define main prior_hci_main
#include "../build/resident-offline/hci-harness.c"
#undef main
int main(void)
{
    reset();g_fd=42;assert(hci_output_idle()); /* Synthetic open descriptor; all I/O is mocked. */
    int calls=complete_calls,reads=read_starts;
    g_pending[IX_ACL_OUT]=1;assert(!hci_output_idle());
    g_pending[IX_ACL_OUT]=0;g_transport_failed=1;assert(!hci_output_idle());
    g_transport_failed=0;g_fd=-1;assert(!hci_output_idle());
    assert(calls==complete_calls && reads==read_starts);
    puts("Output-idle predicate rejects pending, failed and closed state without I/O");return 0;
}
