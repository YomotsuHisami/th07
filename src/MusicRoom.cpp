#include "MusicRoom.hpp"

#include <cstdio>
#include <cstdlib>

#include "AnmIdx.hpp"
#include "AnmManager.hpp"
#include "AsciiManager.hpp"
#include "Chain.hpp"
#include "Controller.hpp"
#include "FileSystem.hpp"
#include "Localization.hpp"
#include "SoundPlayer.hpp"
#include "Supervisor.hpp"

#ifdef TH_DEV_TOOLS
static double AuditMusicRoomGpuAgainstSurface(SDL_Surface *source)
{
    if (!source || source->w != 640 || source->h != 480 ||
        source->format != SDL_PIXELFORMAT_RGBA32)
    {
        return -1.0;
    }

    const i32 width = source->w;
    const i32 height = source->h;
    u8 *readback = new u8[(size_t)width * height * 4];
    g_Supervisor.gfxDevice->ReadPixels(0, 0, width, height, readback);
    const u8 *expected = static_cast<const u8 *>(source->pixels);
    uint64_t absDiff = 0;
    uint64_t compared = 0;
    for (i32 y = 0; y < height; y++)
    {
        const u8 *expectedRow = expected + (size_t)y * source->pitch;
        const u8 *actualRow = readback + (size_t)y * width * 4;
        for (i32 x = 0; x < width; x++)
        {
            for (i32 c = 0; c < 3; c++)
            {
                absDiff += (uint64_t)std::abs((i32)actualRow[x * 4 + c] -
                                             (i32)expectedRow[x * 4 + c]);
                compared++;
            }
        }
    }
    delete[] readback;
    return compared ? (double)absDiff / (double)compared : -1.0;
}
#endif

ZunResult MusicRoom::CheckInputEnable()
{
    i32 i;

    if (this->waitFramesCounter == 0)
    {
        for (i = 0; i < 31; i++)
        {
            if (this->cursor == i)
            {
                this->titleSprites[i].pendingInterrupt = 1;
            }
            else
            {
                this->titleSprites[i].pendingInterrupt = 2;
            }
        }
        for (i = 0; i < 8; i++)
        {
            this->descriptionSprites[i].pendingInterrupt = 1;
        }
    }
    if (this->waitFramesCounter >= 8)
    {
        this->enableInput = 1;
    }
    return ZUN_SUCCESS;
}

i32 MusicRoom::ProcessInput()
{
    char local_54[66];
    i32 i;

    if (WAS_PRESSED_RAW(TH_BUTTON_UP))
    {
        this->cursor--;
        if (this->cursor < 0)
        {
            this->cursor = this->numDescriptors - 1;
            this->listingOffset = this->numDescriptors - 10;
            if (this->listingOffset < 0)
            {
                this->listingOffset = 0;
            }
        }
        else if (this->listingOffset > this->cursor)
        {
            this->listingOffset = this->cursor;
        }
        for (i = 0; i < 31; i++)
        {
            if (this->cursor == i)
            {
                this->titleSprites[i].pendingInterrupt = 1;
            }
            else
            {
                this->titleSprites[i].pendingInterrupt = 2;
            }
        }
    }
    if (WAS_PRESSED_RAW(TH_BUTTON_DOWN))
    {
        this->cursor = this->cursor + 1;
        if (this->cursor >= this->numDescriptors)
        {
            this->cursor = 0;
            this->listingOffset = 0;
        }
        else
        {
            if (this->listingOffset <= this->cursor - 10)
            {
                this->listingOffset = this->cursor - 9;
            }
        }
        for (i = 0; i < 31; i++)
        {
            if (this->cursor == i)
            {
                this->titleSprites[i].pendingInterrupt = 1;
            }
            else
            {
                this->titleSprites[i].pendingInterrupt = 2;
            }
        }
    }
    if (WAS_PRESSED_RAW(TH_BUTTON_SELECTMENU))
    {
        this->selectedIdx = this->cursor;
        if (g_Supervisor.cfg.preloadBgm)
        {
            g_SoundPlayer.StartBGM("thbgm.dat");
        }
        g_Supervisor.PlayAudio(this->trackDescriptors[this->selectedIdx].path);
        for (i = 0; i < 8; i++)
        {
            memset(local_54, 0, sizeof(local_54));
            memcpy(local_54, this->trackDescriptors[this->selectedIdx].description[i], 64);
            if (local_54[0] != '\0')
            {
                this->descriptionSprites[i].active = 1;
                AnmManager::DrawVmTextFmt(g_AnmManager, this->descriptionSprites + i, 0xffe0c0,
                                          0x300000, local_54);
            }
            else
            {
                this->descriptionSprites[i].active = 0;
            }
            this->descriptionSprites[i].pendingInterrupt = 1;
        }
    }
    if (WAS_PRESSED_RAW(TH_BUTTON_RETURNMENU))
    {
        g_Supervisor.curState = 1;
        return 1;
    }

    return 0;
}

u32 MusicRoom::OnUpdate(MusicRoom *arg)
{
    i32 iVar1;
    i32 i;

    arg->UpdatePrev();

    iVar1 = arg->enableInput;
recheck:
    switch (arg->enableInput)
    {
    case 0:
        if (!arg->CheckInputEnable())
        {
            break;
        }
        goto recheck;
    case 1:
        if (arg->ProcessInput())
        {
            return CHAIN_CALLBACK_RESULT_CONTINUE_AND_REMOVE_JOB;
        }
    default:
        break;
    }
    if (iVar1 != arg->enableInput)
    {
        arg->waitFramesCounter = 0;
    }
    else
    {
        arg->waitFramesCounter++;
    }
    g_AnmManager->ExecuteScript(&arg->vm[0]);
    for (i = 0; i < 31; i++)
    {
        g_AnmManager->ExecuteScript(&arg->titleSprites[i]);
    }
    for (i = 0; i < 8; i++)
    {
        g_AnmManager->ExecuteScript(&arg->descriptionSprites[i]);
    }
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

u32 MusicRoom::OnDraw(MusicRoom *arg)
{
    ZunVec3 local_18;
    char local_c[4];
    i32 i;

    local_c[0] = 127;
    local_c[1] = 0;
    g_AnmManager->SetTexture(0);
    g_AnmManager->CopySurfaceToBackBuffer(0, 0, 0, 0, 0);
#ifdef TH_DEV_TOOLS
    static bool auditedBackgroundCopy = false;
    if (!auditedBackgroundCopy && g_AnmManager->surfacesBis[0])
    {
        auditedBackgroundCopy = true;
        ZunViewport viewport;
        g_Supervisor.gfxDevice->GetViewport(viewport);
        SDL_Surface *source = g_AnmManager->surfacesBis[0];
        const i32 width = source->w;
        const i32 height = source->h;
        if (width == 640 && height == 480 && source->format == SDL_PIXELFORMAT_RGBA32)
        {
            const double mae = AuditMusicRoomGpuAgainstSurface(source);
            SDL_Log("th07 music room audit: post-copy viewport=%d,%d %dx%d source=%dx%d "
                    "gpu-mae=%.3f",
                    viewport.x, viewport.y, viewport.width, viewport.height, width, height,
                    mae);
        }
        else
        {
            SDL_Log("th07 music room audit: post-copy unsupported source=%dx%d format=%u "
                    "viewport=%d,%d %dx%d",
                    width, height, (unsigned)source->format, viewport.x, viewport.y,
                    viewport.width, viewport.height);
        }
    }
#endif
    g_AnmManager->DrawInterpNoRotation(&arg->vm[0]);
#ifdef TH_DEV_TOOLS
    static bool auditedStableLayers = false;
    const bool auditStableLayers = !auditedStableLayers && arg->waitFramesCounter >= 90 &&
                                   g_AnmManager->surfacesBis[0];
    if (auditStableLayers)
    {
        g_AnmManager->Flush();
        SDL_Log("th07 music room audit: stable after-vm0 gpu-mae=%.3f",
                AuditMusicRoomGpuAgainstSurface(g_AnmManager->surfacesBis[0]));
    }
#endif
    for (i = arg->listingOffset; i < arg->listingOffset + 10; i++)
    {
        if (i >= arg->numDescriptors)
        {
            break;
        }
        g_AsciiManager.SetColor(arg->titleSprites[i].color.color);
        arg->titleSprites[i].pos.x = 93.0f;
        arg->titleSprites[i].pos.y = (f32)((i + 1 - arg->listingOffset) * 18) + 104.0f - 20.0f;
        arg->titleSprites[i].pos.z = 0.0f;
        g_AnmManager->DrawInterpNoRotation(arg->titleSprites + i);
        local_18 = arg->titleSprites[i].pos;
        local_18.x -= 60.0f;
        if (arg->cursor == i)
        {
            g_AsciiManager.AddString(&local_18, local_c);
        }
        local_18.x += 15.0f;
        AsciiManager::AddFormatText(&g_AsciiManager, &local_18, "%2d.", i + 1);
    }
#ifdef TH_DEV_TOOLS
    if (auditStableLayers)
    {
        g_AnmManager->Flush();
        SDL_Log("th07 music room audit: stable after-title-list gpu-mae=%.3f",
                AuditMusicRoomGpuAgainstSurface(g_AnmManager->surfacesBis[0]));
    }
#endif
    i++;
    for (i = 0; i < 8; i++)
    {
        g_AnmManager->DrawInterpNoRotation(&arg->descriptionSprites[i]);
    }
#ifdef TH_DEV_TOOLS
    if (auditStableLayers)
    {
        g_AnmManager->Flush();
        SDL_Log("th07 music room audit: stable after-descriptions gpu-mae=%.3f",
                AuditMusicRoomGpuAgainstSurface(g_AnmManager->surfacesBis[0]));
        auditedStableLayers = true;
    }
#endif
    g_AsciiManager.color = 0xffffffff;
    return CHAIN_CALLBACK_RESULT_CONTINUE;
}

ZunResult MusicRoom::AddedCallback(MusicRoom *arg)
{
    char lineCharBuffer[66];
    char *firstChar;
    i32 charIdx;
    char *curChar;
    i32 lineIdx;
    i32 offset;
    if (g_AnmManager->LoadSurface(0, "data/result/music.jpg") != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }
    if (g_AnmManager->LoadAnms(ANM_FILE_MUSIC, "data/music00.anm", ANM_OFFSET_MUSIC) != ZUN_SUCCESS)
    {
        return ZUN_ERROR;
    }
#ifdef TH_DEV_TOOLS
    SDL_Log("th07 music room audit: resources loaded, localization=%d",
            Localization::Active() ? 1 : 0);
#endif

    g_AnmManager->SetAnmIdxAndExecuteScript(&arg->vm[0], 2304);
    arg->waitFramesCounter = 0;
    curChar = (char *)FileSystem::OpenFile("data/musiccmt.txt", 0);
    firstChar = curChar;
    if ((u8 *)curChar == NULL)
    {
        return ZUN_ERROR;
    }

    arg->trackDescriptors = new TrackDescriptor[32];
    offset = -1;
    while (((uintptr_t)curChar - (uintptr_t)firstChar) < g_LastFileSize)
    {
        if (*curChar == '@')
        {
            curChar++;
            offset++;
            charIdx = 0;
            while (*curChar != '\n' && *curChar != '\r')
            {
                arg->trackDescriptors[offset].path[charIdx] = *curChar;
                curChar++;
                charIdx++;
                if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                {
                    goto LAB_0043b195;
                }
            }
            while (*curChar == '\n' || *curChar == '\r')
            {
                curChar++;
                if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                {
                    goto LAB_0043b195;
                }
            }
            charIdx = 0;
            while (*curChar != '\n' && *curChar != '\r')
            {
                arg->trackDescriptors[offset].title[charIdx] = *curChar;
                curChar++;
                charIdx++;
                if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                {
                    goto LAB_0043b195;
                }
            }
            // Vanilla TH07 intentionally/actually has the impossible AND here,
            // after the track title (not after the path). It leaves the title
            // terminator in place, so description slot 0 parses as an empty line.
            // base_tsa's music_cmt#line_num hook is first reached while advancing
            // from that empty slot to slot 1; therefore translated comment index 0
            // (the numbered-title "@") belongs to VM slot 1 at y=336.
            while (*curChar == '\n' && *curChar == '\r')
            {
                curChar++;
                if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                {
                    goto LAB_0043b195;
                }
            }
            for (lineIdx = 0; lineIdx < 8; lineIdx++)
            {
                if (*curChar == '@')
                {
                    break;
                }

                memset(arg->trackDescriptors[offset].description[lineIdx], 0,
                       sizeof(arg->trackDescriptors[offset].description[lineIdx]));
                charIdx = 0;
                while (*curChar != '\n' && *curChar != '\r')
                {
                    arg->trackDescriptors[offset].description[lineIdx][charIdx] = *curChar;
                    curChar++;
                    charIdx++;
                    if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                    {
                        goto LAB_0043b195;
                    }
                }
                while (*curChar == '\n' || *curChar == '\r')
                {
                    curChar++;
                    if (((uintptr_t)curChar - (uintptr_t)firstChar) >= g_LastFileSize)
                    {
                        goto LAB_0043b195;
                    }
                }
            }
        }
        else
        {
            curChar++;
        }
    }
LAB_0043b195:
    arg->numDescriptors = offset + 1;
    if (Localization::Active())
    {
        for (i32 track = 1; track <= arg->numDescriptors; track++)
        {
            TrackDescriptor &descriptor = arg->trackDescriptors[track - 1];
            const char *title = Localization::MusicTitle(track, descriptor.title);
            Localization::CopyText(descriptor.title, sizeof(descriptor.title), title);
            // Slot 0 is the empty line preserved by the vanilla parser above.
            // thcrap line_num 0 is published only when the original loop
            // advances to slot 1, so musiccmt.js index N maps to VM slot N+1.
            for (i32 slot = 1; slot < 8; slot++)
            {
                const i32 line = slot - 1;
                const char *comment = Localization::MusicComment(
                    track, static_cast<std::uint16_t>(line), descriptor.description[slot]);
                // thcrap resolves comment index 0's "@" marker through the
                // Music Room Numbered Title format while rendering VM slot 1.
                if (std::strcmp(comment, "@") == 0)
                    std::snprintf(descriptor.description[slot],
                                  sizeof(descriptor.description[slot]),
                                  "No. %2u  %s", static_cast<unsigned>(track), descriptor.title);
                else
                    Localization::CopyText(descriptor.description[slot],
                                           sizeof(descriptor.description[slot]), comment);
            }
        }
    }
    for (offset = 0; offset < arg->numDescriptors; offset++)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(&arg->titleSprites[offset], offset + 2305);
        AnmManager::DrawVmTextFmt(g_AnmManager, arg->titleSprites + offset, 0xc0e0ff, 0x302080,
                                  arg->trackDescriptors[offset].title);
        arg->titleSprites[offset].pos.x = 93.0f;
        arg->titleSprites[offset].pos.y = (f32)((offset + 1) * 18) + 104.0f - 20.0f;
        arg->titleSprites[offset].pos.z = 0.0f;
        arg->titleSprites[offset].anchor = 3;
    }
    for (offset = 0; offset < 8; offset++)
    {
        g_AnmManager->SetAnmIdxAndExecuteScript(&arg->descriptionSprites[offset], offset + 1799);
        memset(lineCharBuffer, 0, sizeof(lineCharBuffer));
        memcpy(lineCharBuffer, arg->trackDescriptors[arg->selectedIdx].description[offset], 64);
        if (*lineCharBuffer != '\0')
        {
            arg->descriptionSprites[offset].active = 1;
            AnmManager::DrawVmTextFmt(g_AnmManager, arg->descriptionSprites + offset, 0xffe0c0,
                                      0x300000, (char *)&lineCharBuffer);
        }
        else
        {
            arg->descriptionSprites[offset].active = 0;
        }
    }
    free(firstChar);
#ifdef TH_DEV_TOOLS
    SDL_Log("th07 music room audit: ready descriptors=%d selected=%d",
            arg->numDescriptors, arg->selectedIdx);
#endif
    return ZUN_SUCCESS;
}

ZunResult MusicRoom::DeletedCallback(MusicRoom *arg)
{
    delete[] arg->trackDescriptors;
    arg->trackDescriptors = NULL;
    g_AnmManager->ReleaseSurface(0);
    g_AnmManager->ReleaseAnm(46);
    g_AnmManager->ReleaseAnm(47);
    g_Chain.Cut(arg->drawChain);
    arg->drawChain = NULL;
    return ZUN_SUCCESS;
}

ZunResult MusicRoom::RegisterChain()
{
    static MusicRoom g_MusicRoom;
    MusicRoom *musicRoom = &g_MusicRoom;

    musicRoom->calcChain = g_Chain.CreateElem((ChainCallback)OnUpdate);
    musicRoom->calcChain->arg = musicRoom;
    musicRoom->calcChain->addedCallback = (ChainLifecycleCallback)AddedCallback;
    musicRoom->calcChain->deletedCallback = (ChainLifecycleCallback)DeletedCallback;
    if (g_Chain.AddToCalcChain(musicRoom->calcChain, 3))
    {
        return ZUN_ERROR;
    }

    musicRoom->drawChain = g_Chain.CreateElem((ChainCallback)OnDraw);
    musicRoom->drawChain->arg = musicRoom;
    g_Chain.AddToDrawChain(musicRoom->drawChain, 0);
    return ZUN_SUCCESS;
}
