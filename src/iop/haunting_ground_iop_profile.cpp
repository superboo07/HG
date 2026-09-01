#include "iop_service.h"
#include "module_factories.h"

namespace ps2x::iop::detail
{
    ProfileDefinition createHauntingGroundProfile()
    {
        // SLUS_210.75 links CRI DTX 1.08 and binds its transport through
        // SID 0x90000200. The compatibility-owned address ranges below are
        // deliberately outside the loaded ELF's 0x00100000-0x01992000 range.
        // Guest URPC table/dispatcher bindings remain disabled until their
        // exact build-scoped addresses are established from the ELF.
        CriDtxBindings bindings{
            .serviceName = "Haunting Ground CRI DTX",
            .sid = 0x90000200u,
            .urpcObjectBase = 0x01F20000u,
            .urpcObjectLimit = 0x01F28000u,
            .urpcObjectStride = 0x20u,
            .urpcFunctionTableBase = 0x01F2F000u,
            .urpcObjectTableBase = 0x01F2F100u,
            .dispatcherFunctionAddress = 0x01F2F200u,
            .rpcServerPoolBase = 0x01F28000u,
            .rpcServerStride = 0x80u,
        };

        // sceCdSearchFile's IOP RPC returns the disc directory metadata in the
        // caller's 300-byte work buffer. These values are facts from the exact
        // SLUS_210.75 disc build, and the host file size is checked before the
        // record is exposed to the guest.
        CdSearchFileBindings cdSearchBindings{
            .serviceName = "Haunting Ground CD search",
            .sid = 0x80000597u,
            .function = 0u,
            .pathOffset = 0x24u,
            .pathBytes = 0x100u,
            .resultBytes = 0x24u,
            .receiveResultOffset = 0u,
            .successValue = 1u,
            .failureValue = 0u,
            .validateHostFileSize = true,
            .files = {
                {
                    .guestPath = "\\DATA.CVM;1",
                    .lsn = 0x00164972u,
                    .size = 0x5E018000u,
                    .date = {0x00u, 0x2Au, 0x25u, 0x0Au,
                             0x06u, 0x01u, 0xD5u, 0x07u},
                },
            },
        };

        // Capcom's SNDDRV command service uses SID 0x77777777. Its command
        // protocol remains only partially characterized, so retain the narrow
        // compatibility bridge without attributing it to CRI or MODHSYN.
        SoundUpdateStubBindings soundBindings{
            .serviceName = "Haunting Ground SNDDRV command compatibility",
            .sid = 0x77777777u,
            .activeStreamCountOffset = 0u,
            .responseCounterOffset = 4u,
            .zeroReceiveBuffer = true,
            .signalNowaitCompletion = false,
            .completeQueuedPlayStreams = false,
            .suppressedCompletionCallbacks = {},
        };

        // SNDDRV.IRX SHA-256
        // 3b8e76b6fcaa1b3fbe59848a6094bd230d5fe9643496e3aff5f3b969730404bf
        // registers SID 0x77777778 at IOP VA 0xA4F4. Its 0x0012xxxx handler
        // copies words 2-4 from the 32-byte request into sceSdVoiceTrans and
        // waits for completion. The live EE caller selects the 100-us polling
        // path with word 5 equal to one.
        CapcomSnddrvTransferBindings transferBindings{
            .serviceName = "Haunting Ground Capcom SNDDRV transfer",
            .sid = 0x77777778u,
            .functionClass = 0x00120000u,
            .maximumTransferBytes = 0x4000u,
            .channel = 0,
            .iopClockHz = 36'864'000u,
            .dmaCyclesPer16BitWord = 4u,
            .pollDelayMicroseconds = 100u,
            .blockingDelayMicroseconds = 1u,
            // The exact driver owns two 0x880-byte bidirectional mailboxes.
            // Its linked worker consumes an odd EE generation and returns the
            // next even generation at tail word 15. Only the proven empty
            // heartbeat and the exact observed one-record opcode-8 start and
            // opcode-5 zero-argument control transactions are characterized.
            .mailboxBytes = 0x880u,
            .mailboxSequenceOffset = 0x87Cu,
            .mailboxCommandIopDestination = 0x000B2780u,
            .mailboxResponseIopDestination = 0x00070E40u,
            // Each of the two demonstrated descriptor pairs owns distinct EE
            // callback objects.  The guest-initialized handle records link
            // 0x003D4DD8/0x003D4DEC to queues 0x003D3FE0/0x003D4010 and
            // 0x003D4E00/0x003D4E14 to queues 0x003D4040/0x003D4070.
            // PCSX2 2.6.3 oracle captures show wraparound split at 16 KiB.
            .playbackDescriptorHandles = {{{0x01F20020u, 0x01F20060u},
                                           {0x01F200A0u, 0x01F200E0u}}},
            .playbackCallbackObjects = {{{0x003D4DD8u, 0x003D4DECu},
                                         {0x003D4E00u, 0x003D4E14u}}},
            .playbackRingBytes = 0x4000u,
            .mailboxIopDestinations = {0x00070E40u, 0x000B2780u},
        };

        return {
            "haunting-ground-us",
            "hg-project",
            {
                .elfName = "SLUS_210.75",
                .entryPoint = 0x00100008u,
                .crc32 = 0xA295AF2Bu,
            },
            [bindings, cdSearchBindings, soundBindings, transferBindings](IopHost &host, const GameIdentity &)
            {
                ServiceList services;
                services.emplace_back(createCriDtxService(host, bindings));
                services.emplace_back(createCdSearchFileService(host, cdSearchBindings));
                services.emplace_back(createSoundUpdateStubService(host, soundBindings));
                services.emplace_back(createCapcomSnddrvTransferService(host, transferBindings));
                return services;
            },
        };
    }
}
