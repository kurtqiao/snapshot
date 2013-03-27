/*++ @file
A screen snap shot app
@KurtQiao
2013.3.27
base on UDK2010
**/

#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleTextInEx.h>

#include <protocol/GraphicsOutput.h>
#include <protocol/UgaDraw.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/ConsoleControl.h>


#pragma pack(1)
/* This should be compatible with EFI_UGA_PIXEL */
typedef struct {
    UINT8 b;
    UINT8 g;
    UINT8 r;
    UINT8 a;
} BMPIXEL;

typedef struct {
    UINTN       Width;
    UINTN       Height;
    BOOLEAN     HasAlpha;
    BMPIXEL    *PixelData;
} BMIMAGE;

typedef struct {
    CHAR8         CharB;
    CHAR8         CharM;
    UINT32        bfSize;
    UINT16        Reserved[2];
    UINT32        ImageOffset;
    UINT32        HeaderSize;
    UINT32        PixelWidth;
    UINT32        PixelHeight;
    UINT16        Planes;             // Must be 1
    UINT16        BitPerPixel;        // 1, 4, 8, or 24
    UINT32        CompressionType;
    UINT32        ImageSize;          // Compressed image size in bytes
    UINT32        XPixelsPerMeter;
    UINT32        YPixelsPerMeter;
    UINT32        NumberOfColors;
    UINT32        ImportantColors;
} BMP_IMAGE_HEADER; 
#pragma pack()

// Console defines and variables
static EFI_GUID EfiConsoleControlProtocolGuid= EFI_CONSOLE_CONTROL_PROTOCOL_GUID;
static EFI_CONSOLE_CONTROL_PROTOCOL *ConsoleControl = NULL;
static EFI_UGA_DRAW_PROTOCOL *UgaDraw = NULL;
static EFI_GRAPHICS_OUTPUT_PROTOCOL *GraphicsOutput = NULL;

static BOOLEAN mHasGraphics = FALSE;
static UINTN mScreenWidth  = 800;
static UINTN mScreenHeight = 600;

VOID 
mSetGraphicsModeEnabled(IN BOOLEAN Enable);

EFI_STATUS 
InitScreen(VOID);

VOID 
mScreenShot(VOID);

BMIMAGE * 
mCreateImage(IN UINTN Width, IN UINTN Height, IN BOOLEAN HasAlpha);

VOID 
mFreeImage(IN BMIMAGE *Image);

EFI_STATUS 
mSaveFile(IN CHAR16 *FileName,IN UINT8 *FileData, IN UINTN FileDataLength);

VOID 
mEncodeBMP(IN BMIMAGE *Image, OUT UINT8 **FileDataReturn, OUT UINTN *FileDataLengthReturn);

EFI_STATUS
EFIAPI
NotificationFunction(
  IN EFI_KEY_DATA *KeyData
  )
{
  mScreenShot();
  Print(L"screen shot ok!\n");
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
InitializeSnapShot(
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *SimpleEx;
  EFI_KEY_DATA                      KeyData;
  EFI_HANDLE                        NotifyHandle;


  Status = InitScreen();
  if (EFI_ERROR (Status)) {
    Print(L"init screen to graphic mode fail!\n");
    return Status;
    }

  Status = gBS->OpenProtocol(
    gST->ConsoleInHandle,
    &gEfiSimpleTextInputExProtocolGuid,
    (VOID**)&SimpleEx,
    gImageHandle,
    NULL,
    EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  if (EFI_ERROR (Status)) {
    Print(L"open simple SimpleTextInEx protocol fail.\n");
    return Status;
    }

  KeyData.KeyState.KeyToggleState = 0;
  KeyData.Key.ScanCode            = 0;
  KeyData.KeyState.KeyShiftState  = EFI_SHIFT_STATE_VALID|EFI_LEFT_CONTROL_PRESSED;
  KeyData.Key.UnicodeChar         = L'w';

  Status = SimpleEx->RegisterKeyNotify(
    SimpleEx,
    &KeyData,
    NotificationFunction,
    &NotifyHandle);
  if (EFI_ERROR (Status)) {
    Print(L"RegisterKeyNotify fail.\n");
    return Status;
    }
  Print(L"Press left ctrl+w to capture screen!\n");

  return Status;
}



VOID mSetGraphicsModeEnabled(IN BOOLEAN Enable)
{
    EFI_CONSOLE_CONTROL_SCREEN_MODE CurrentMode;
    EFI_CONSOLE_CONTROL_SCREEN_MODE NewMode;
    
    if (ConsoleControl != NULL) {
        ConsoleControl->GetMode(ConsoleControl, &CurrentMode, NULL, NULL);
        
        NewMode = Enable ? EfiConsoleControlScreenGraphics
                         : EfiConsoleControlScreenText;
        if (CurrentMode != NewMode)
            ConsoleControl->SetMode(ConsoleControl, NewMode);
    }
}

EFI_STATUS
InitScreen(){
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&EfiConsoleControlProtocolGuid, NULL, (VOID**)&ConsoleControl);
  if (EFI_ERROR (Status)) {
    Print(L"ConsoleControl not support!");
    return EFI_UNSUPPORTED;
  }

  UgaDraw = NULL;
  //
  // Try to open GOP first
  //
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID**)&GraphicsOutput);
  if (EFI_ERROR (Status)) {
    GraphicsOutput = NULL;
    //
    // Open GOP failed, try to open UGA
    //
    Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiUgaDrawProtocolGuid, (VOID**)&UgaDraw);
    if (EFI_ERROR (Status)) {
    Print(L"GraphicsOutput not support!");
    return EFI_UNSUPPORTED;
    }
  }

  mSetGraphicsModeEnabled(TRUE);

  // get screen size
    mHasGraphics = FALSE;
    if (GraphicsOutput != NULL) {
        mScreenWidth = GraphicsOutput->Mode->Info->HorizontalResolution;
        mScreenHeight = GraphicsOutput->Mode->Info->VerticalResolution;
        mHasGraphics = TRUE;
        Print(L"Init screen to graphic mode!\n");
        Print(L"screen width: %d, height: %d \n", mScreenWidth,mScreenHeight);
    } 

  return Status;

}

BMIMAGE * mCreateImage(IN UINTN Width, IN UINTN Height, IN BOOLEAN HasAlpha)
{
    BMIMAGE        *NewImage;
    
    NewImage = (BMIMAGE *) AllocateZeroPool(sizeof(BMIMAGE));
    if (NewImage == NULL)
        return NULL;
    NewImage->PixelData = (BMPIXEL *) AllocateZeroPool(Width * Height * sizeof(BMPIXEL));
    if (NewImage->PixelData == NULL) {
        FreePool(NewImage);
        return NULL;
    }
    
    NewImage->Width = Width;
    NewImage->Height = Height;
    NewImage->HasAlpha = HasAlpha;
    return NewImage;
}

VOID mFreeImage(IN BMIMAGE *Image)
{
    if (Image != NULL) {
        if (Image->PixelData != NULL)
            FreePool(Image->PixelData);
        FreePool(Image);
    }
}

EFI_STATUS mSaveFile(IN CHAR16 *FileName,
                      IN UINT8 *FileData, IN UINTN FileDataLength)
{
    EFI_STATUS          Status;
    EFI_FILE_PROTOCOL   *FileProtocol, *FileHandle;
    UINTN               BufferSize;
    
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFileSystem;

  Status = gBS->LocateProtocol(
                                &gEfiSimpleFileSystemProtocolGuid,
                                NULL,
                                (VOID**)&SimpleFileSystem
                                );
  if (EFI_ERROR(Status)) {
      return (EFI_NOT_FOUND);
    }
  Status = SimpleFileSystem->OpenVolume(SimpleFileSystem, &FileProtocol);
  if (EFI_ERROR(Status)) return Status;
    
  Status = FileProtocol->Open(FileProtocol, 
                              &FileHandle, 
                              FileName,
                              EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                              0);
    if (EFI_ERROR(Status))
        return Status;
    
    BufferSize = FileDataLength;
    Status = FileProtocol->Write(FileHandle, &BufferSize, FileData);
    FileProtocol->Close(FileHandle);
    
    return Status;
}


VOID mEncodeBMP(IN BMIMAGE *Image, OUT UINT8 **FileDataReturn, OUT UINTN *FileDataLengthReturn)
{
    BMP_IMAGE_HEADER    *BmpHeader;
    UINT8               *FileData;
    UINTN               FileDataLength;
    UINT8               *ImagePtr;
    UINT8               *ImagePtrBase;
    UINTN               ImageLineOffset;
    BMPIXEL             *PixelPtr;
    UINTN               x, y;

    ImageLineOffset = Image->Width * 3;
    if ((ImageLineOffset % 4) != 0)
        ImageLineOffset = ImageLineOffset + (4 - (ImageLineOffset % 4));

    // allocate buffer for file data
    FileDataLength = sizeof(BMP_IMAGE_HEADER) + Image->Height * ImageLineOffset;
    FileData = AllocateZeroPool(FileDataLength);
    if (FileData == NULL) {
        Print(L"Error allocate %d bytes\n", FileDataLength);
        *FileDataReturn = NULL;
        *FileDataLengthReturn = 0;
        return;
    }
    
    // fill header
    BmpHeader = (BMP_IMAGE_HEADER *)FileData;
    BmpHeader->CharB = 'B'; //0x42
    BmpHeader->CharM = 'M'; //0x4D
    BmpHeader->bfSize = (UINT32)FileDataLength;
    BmpHeader->ImageOffset = sizeof(BMP_IMAGE_HEADER);
    BmpHeader->HeaderSize = 40;
    BmpHeader->PixelWidth = (UINT32)Image->Width;
    BmpHeader->PixelHeight = (UINT32)Image->Height;
    BmpHeader->Planes = 1;
    BmpHeader->BitPerPixel = 24;
    BmpHeader->CompressionType = 0;
    BmpHeader->XPixelsPerMeter = 0;//0xb13;
    BmpHeader->YPixelsPerMeter = 0;//0xb13;
    
    // fill pixel buffer
    ImagePtrBase = FileData + BmpHeader->ImageOffset;
    for (y = 0; y < Image->Height; y++) {
        ImagePtr = ImagePtrBase;
        ImagePtrBase += ImageLineOffset;
        PixelPtr = Image->PixelData + (Image->Height - 1 - y) * Image->Width;
        
        for (x = 0; x < Image->Width; x++) {
            *ImagePtr++ = PixelPtr->b;
            *ImagePtr++ = PixelPtr->g;
            *ImagePtr++ = PixelPtr->r;
            PixelPtr++;
        }
    }
    
    *FileDataReturn = FileData;
    *FileDataLengthReturn = FileDataLength;
}

//
// Make a screenshot
//

VOID mScreenShot(VOID)
{
    EFI_STATUS      Status;
    BMIMAGE        *Image;
    UINT8           *FileData;
    UINTN           FileDataLength;
    UINTN           Index;
    
    if (!mHasGraphics)
        return;
    
    // allocate a buffer for the whole screen
    Image = mCreateImage(mScreenWidth, mScreenHeight, FALSE);
    if (Image == NULL) {
        Print(L"Error mCreateImage returned NULL\n");
        goto bailout_wait;
    }
    
    // get full screen image
    if (GraphicsOutput != NULL) {
        GraphicsOutput->Blt(GraphicsOutput, 
                            (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)Image->PixelData, 
                            EfiBltVideoToBltBuffer,
                            0, 
                            0, 
                            0, 
                            0, 
                            Image->Width, 
                            Image->Height, 
                            0);
    }
    else if (UgaDraw != NULL) {
        UgaDraw->Blt(UgaDraw, (EFI_UGA_PIXEL *)Image->PixelData, EfiUgaVideoToBltBuffer,
                     0, 0, 0, 0, Image->Width, Image->Height, 0);
    }

    Print(L"Start screen shot...\n");
    // encode as BMP
    mEncodeBMP(Image, &FileData, &FileDataLength);
    mFreeImage(Image);
    if (FileData == NULL) {
        Print(L"Error mEncodeBMP returned NULL\n");
        goto bailout_wait;
    }
    
    // save to file 
    Status = mSaveFile(L"screenshot.bmp", FileData, FileDataLength);
    FreePool(FileData);
    if (EFI_ERROR(Status)) {
        Print(L"Error mSaveFile: %x\n", Status);
        goto bailout_wait;
    }
    
    return;
    
    // DEBUG: switch to text mode
bailout_wait:
    mSetGraphicsModeEnabled(FALSE);
    gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &Index);
}