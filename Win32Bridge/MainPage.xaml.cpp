//
// MainPage.xaml.cpp
// Implementação da classe MainPage.
//

#include "pch.h"
#include "MainPage.xaml.h"
#include "Bridge\\CompatibilityCatalog.h"
#include "Bridge\\GuestRuntime.h"
#include "Bridge\\GuestStorage.h"
#include "Bridge\\GuestWindow.h"
#include "Bridge\\ImportBinder.h"
#include "Bridge\\Win32Shims.h"
#include "Bridge/RuntimeDiagnostics.h"
#include "GuestApps\\DialogDemoEmbedded.h"

#include <agile.h>
#include <atomic>

using namespace Win32Bridge;

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Controls::Primitives;
using namespace Windows::UI::Xaml::Data;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using namespace Windows::UI::Xaml::Navigation;
using namespace Windows::UI::Core;
using namespace Windows::ApplicationModel;
using namespace Windows::Security::Cryptography;
using namespace Windows::Storage;
using namespace Windows::Storage::Pickers;
using namespace Windows::Storage::Streams;
using namespace Windows::System::Threading;
using namespace concurrency;

namespace
{
	std::atomic<bool> g_runtimeReportWriteInFlight{ false };

	void SaveRuntimeReportSnapshot()
	{
		// The periodic timer, the worker thread and the final completion path
		// can all request a snapshot.  ReplaceExisting is not an atomic
		// read-modify-write operation, so overlapping writes can leave the log
		// truncated to zero bytes.  Allow one complete write at a time.
		if (g_runtimeReportWriteInFlight.exchange(true))
		{
			return;
		}
		const auto text = ref new String(Win32Bridge::Bridge::RuntimeDiagnostics::Snapshot().c_str());
		create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
			"Win32Bridge-runtime.log", CreationCollisionOption::OpenIfExists))
			.then([text](StorageFile^ file)
		{
			return create_task(file->OpenAsync(FileAccessMode::ReadWrite));
		})
			.then([text](IRandomAccessStream^ stream)
		{
			// FileIO::WriteTextAsync uses a temporary replacement file. That
			// is normally fine, but its internal missing-temp-file probe becomes
			// a disruptive first-chance COMException under the VS debugger.
			// The report is diagnostic data, so a direct, serialized overwrite is
			// both sufficient and much calmer while stepping through a guest.
			stream->Size = 0;
			stream->Seek(0);
			auto writer = ref new DataWriter(stream);
			writer->UnicodeEncoding = UnicodeEncoding::Utf8;
			writer->WriteString(text);
			return create_task(writer->StoreAsync()).then([writer, stream](unsigned int)
			{
				delete writer;
				delete stream;
			});
		})
			.then([](task<void> completed)
		{
				try { completed.get(); }
				catch (Exception^ error)
				{
					Win32Bridge::Bridge::RuntimeDiagnostics::Record(
						L"HOST LOG EXCEPTION: runtime-report write failed; HRESULT " +
						std::to_wstring(static_cast<unsigned long>(error->HResult)) + L".");
				}
				catch (...)
				{
					Win32Bridge::Bridge::RuntimeDiagnostics::Record(
						L"HOST LOG EXCEPTION: runtime-report write raised an unknown exception.");
				}
				g_runtimeReportWriteInFlight.store(false);
			});
	}

    const wchar_t* DispositionLabel(Win32Bridge::Bridge::ImportDisposition disposition)
    {
        using Win32Bridge::Bridge::ImportDisposition;
        switch (disposition)
        {
        case ImportDisposition::NeedsBridge: return L"ADAPTER";
        case ImportDisposition::Deferred: return L"PENDING";
        default: return L"UNSUPPORTED";
        }
    }
}

// O modelo de item de Página em Branco está documentado em https://go.microsoft.com/fwlink/?LinkId=402352&clcid=0x416

MainPage::MainPage()
{
	InitializeComponent();
	Bridge::MouseInput().Attach(Window::Current->CoreWindow);
	auto surfaceHost = dynamic_cast<Panel^>(FindName("GuestSurfaceHost"));
	if (surfaceHost)
	{
		m_guestWindows = std::make_shared<Bridge::GuestWindowManager>(
			Window::Current->CoreWindow,
			surfaceHost);
	}
	m_guestStorage = std::make_shared<Bridge::GuestStorageContext>(
		ApplicationData::Current->LocalFolder,
		L"C:\\Program Files\\Win32Bridge\\DialogDemo.exe");

	// The bootstrap PE is compiled into the host, then persisted in LocalFolder.  This
	// keeps guest staging independent from AppX content-copy rules on the target.
	const auto bootstrapBytes = CryptographicBuffer::DecodeFromBase64String(GuestAssets::DialogDemoBase64());
	create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync("drive_c", CreationCollisionOption::OpenIfExists))
		.then([](StorageFolder^ drive)
	{
		return drive->CreateFolderAsync("Program Files", CreationCollisionOption::OpenIfExists);
	})
		.then([](StorageFolder^ programFiles)
	{
		return programFiles->CreateFolderAsync("Win32Bridge", CreationCollisionOption::OpenIfExists);
	})
		.then([bootstrapBytes](StorageFolder^ guestDirectory)
	{
		return create_task(guestDirectory->CreateFileAsync("DialogDemo.exe", CreationCollisionOption::ReplaceExisting))
			.then([bootstrapBytes](StorageFile^ file)
		{
				return create_task(FileIO::WriteBufferAsync(file, bootstrapBytes))
					.then([file]() { return file; });
			});
	})
		.then([](StorageFile^ file)
	{
		return FileIO::ReadBufferAsync(file);
	})
		.then([this](IBuffer^ buffer)
	{
		Platform::Array<byte>^ bytes = ref new Platform::Array<byte>(buffer->Length);
		DataReader::FromBuffer(buffer)->ReadBytes(bytes);
		m_guestBytes = bytes;

		Bridge::PeImageInfo image;
		if (!Bridge::PeImage::Inspect(bytes->Data, bytes->Length, &image))
		{
			GuestStatusText->Text = "Guest inspection failed.";
			ApiReportText->Text = ref new String(image.error.c_str());
			return;
		}

		Bridge::MappedPeImage mappedImage;
		std::wstring mappingError;
		if (!Bridge::PeMapper::Materialize(bytes->Data, bytes->Length, &mappedImage, &mappingError) ||
			!Bridge::PeMapper::ApplyBaseRelocations(&mappedImage, mappedImage.preferredImageBase, &mappingError))
		{
			GuestStatusText->Text = "Guest mapping failed.";
			ApiReportText->Text = ref new String(mappingError.c_str());
			return;
		}

		Bridge::BindingReport binding;
		std::wstring bindingError;
		if (!Bridge::ImportBinder::Bind(&mappedImage, image, Bridge::ResolveRuntimeImport, &binding, &bindingError))
		{
			GuestStatusText->Text = "Guest import binding failed.";
			ApiReportText->Text = ref new String(bindingError.c_str());
			return;
		}

		unsigned int adapters = 0;
		unsigned int deferred = 0;
		std::wstring report = L"PE32+ x64\nEntry point RVA (decimal): " + std::to_wstring(image.entryPointRva) + L"\n\n";
		for (size_t index = 0; index < image.imports.size(); ++index)
		{
			const auto& symbol = image.imports[index];
			const auto& resolution = binding.resolutions[index];
			if (resolution.disposition == Bridge::ImportDisposition::NeedsBridge) ++adapters;
			if (resolution.disposition == Bridge::ImportDisposition::Deferred) ++deferred;

			report += L"[";
			report += DispositionLabel(resolution.disposition);
			report += L"] ";
			report += symbol.library;
			report += L"!";
			report += symbol.importedByOrdinal ? L"#" + std::to_wstring(symbol.ordinal) : symbol.name;
			report += L"\n    ";
			report += resolution.note;
			report += L"\n";
		}

		const std::wstring summary = L"x64 PE accepted. " + std::to_wstring(image.imports.size()) +
			L" imports; IAT: " + std::to_wstring(binding.bound) + L" bound, " +
			std::to_wstring(binding.unresolved) + L" unresolved. Catalog: " +
			std::to_wstring(adapters) + L" adapter(s), " + std::to_wstring(deferred) + L" pending.";
		GuestStatusText->Text = ref new String((L"Guest staged in LocalFolder\\drive_c. " + summary).c_str());
		RunGuestButton->IsEnabled = binding.unresolved == 0;
		ApiReportText->Text = ref new String(report.c_str());
		SaveImportReport(L"Win32Bridge import report\n" + summary + L"\n\n" + report);
	})
		.then([this](task<void> previous)
	{
			try
			{
				previous.get();
			}
			catch (Platform::Exception^ error)
			{
				GuestStatusText->Text = "Could not stage the embedded guest in LocalFolder\\drive_c. Error: " + error->Message;
			}
		});

	Stage7Zip();
}

void MainPage::Stage7Zip()
{
	Run7ZipButton->IsEnabled = false;
	create_task(Package::Current->InstalledLocation->GetFolderAsync("GuestApps"))
		.then([](StorageFolder^ guestApps)
	{
		return guestApps->GetFolderAsync("7-zip");
	})
		.then([](StorageFolder^ sourceFolder)
	{
		return create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync("drive_c", CreationCollisionOption::OpenIfExists))
			.then([](StorageFolder^ drive) { return drive->CreateFolderAsync("Program Files", CreationCollisionOption::OpenIfExists); })
			.then([sourceFolder](StorageFolder^ programFiles)
		{
			return create_task(programFiles->CreateFolderAsync("7-zip", CreationCollisionOption::OpenIfExists))
				.then([sourceFolder](StorageFolder^ stagedFolder)
			{
					auto copyFile = [sourceFolder, stagedFolder](const wchar_t* name)
					{
						return create_task(sourceFolder->GetFileAsync(ref new String(name)))
							.then([stagedFolder](StorageFile^ source)
						{
							return create_task(source->CopyAsync(stagedFolder, source->Name, NameCollisionOption::ReplaceExisting));
						});
					};

					// 7zFM loads these sibling modules at runtime.  Keep the full
					// executable set together in the virtual guest directory.
					return copyFile(L"7zG.exe")
						.then([copyFile](StorageFile^) { return copyFile(L"7zFM.exe"); })
						.then([copyFile](StorageFile^) { return copyFile(L"7z.dll"); })
						.then([copyFile](StorageFile^) { return copyFile(L"7-zip.dll"); })
						.then([stagedFolder](StorageFile^) { return stagedFolder->GetFileAsync("7zG.exe"); });
				});
			});
	})
		.then([](StorageFile^ guest)
	{
		return FileIO::ReadBufferAsync(guest);
	})
		.then([this](IBuffer^ buffer)
	{
		m_sevenZipBytes = ref new Platform::Array<byte>(buffer->Length);
		DataReader::FromBuffer(buffer)->ReadBytes(m_sevenZipBytes);
		m_sevenZipStorage = std::make_shared<Bridge::GuestStorageContext>(
			ApplicationData::Current->LocalFolder,
			L"C:\\Program Files\\7-zip\\7zG.exe");
		Run7ZipButton->IsEnabled = true;
	})
		.then([this](task<void> previous)
	{
		try
		{
			previous.get();
		}
		catch (Platform::Exception^ error)
		{
			GuestStatusText->Text = "Could not stage the packaged 7-Zip tree. Error: " + error->Message;
		}
	});
}

void MainPage::Run7Zip_Click(Object^, RoutedEventArgs^)
{
	if (!m_sevenZipBytes || !m_sevenZipStorage)
	{
		GuestStatusText->Text = "7-Zip is not staged yet.";
		return;
	}

	m_guestBytes = m_sevenZipBytes;
	m_guestStorage = m_sevenZipStorage;
	PrepareGuest(m_guestBytes, L"C:\\Program Files\\7-zip\\7zG.exe", L"7zG.exe");
	if (RunGuestButton->IsEnabled)
	{
		RunGuest_Click(nullptr, nullptr);
	}
}

void MainPage::Test7Zip_Click(Object^, RoutedEventArgs^)
{
	// This intentionally does not stage or overwrite content. It verifies the
	// persistent virtual C: drive that every guest receives at runtime.
	Test7ZipButton->IsEnabled = false;
	GuestStatusText->Text = "Loading 7zFM.exe from LocalFolder\\drive_c...";

	create_task(ApplicationData::Current->LocalFolder->GetFolderAsync("drive_c"))
		.then([](StorageFolder^ drive) { return drive->GetFolderAsync("Program Files"); })
		.then([](StorageFolder^ programFiles) { return programFiles->GetFolderAsync("7-zip"); })
		.then([](StorageFolder^ sevenZip) { return sevenZip->GetFileAsync("7zFM.exe"); })
		.then([](StorageFile^ executable) { return FileIO::ReadBufferAsync(executable); })
		.then([this](IBuffer^ buffer)
	{
		Platform::Array<byte>^ bytes = ref new Platform::Array<byte>(buffer->Length);
		DataReader::FromBuffer(buffer)->ReadBytes(bytes);
		PrepareGuest(bytes, L"C:\\Program Files\\7-zip\\7zFM.exe", L"7zFM.exe from LocalStorage");
		if (RunGuestButton->IsEnabled)
		{
			RunGuest_Click(nullptr, nullptr);
		}
	})
		.then([this](task<void> previous)
	{
		try
		{
			previous.get();
		}
		catch (Platform::Exception^ error)
		{
			GuestStatusText->Text = "7zFM.exe was not found in LocalFolder\\drive_c\\Program Files\\7-zip. Error: " + error->Message;
		}
		Test7ZipButton->IsEnabled = true;
	});
}

void MainPage::LoadGuest_Click(Object^, RoutedEventArgs^)
{
	FileOpenPicker^ picker = ref new FileOpenPicker();
	picker->SuggestedStartLocation = PickerLocationId::DocumentsLibrary;
	picker->FileTypeFilter->Append(".exe");
	LoadGuestButton->IsEnabled = false;
	GuestStatusText->Text = "Choose a 64-bit Windows executable to stage.";

	create_task(picker->PickSingleFileAsync())
		.then([this](StorageFile^ source)
	{
		if (source == nullptr)
		{
			GuestStatusText->Text = "Custom guest selection canceled.";
			LoadGuestButton->IsEnabled = true;
			return task_from_result<StorageFile^>(nullptr);
		}

		GuestStatusText->Text = "Copying custom guest into LocalFolder\\drive_c...";
		return create_task(ApplicationData::Current->LocalFolder->CreateFolderAsync("drive_c", CreationCollisionOption::OpenIfExists))
			.then([](StorageFolder^ drive) { return drive->CreateFolderAsync("Program Files", CreationCollisionOption::OpenIfExists); })
			.then([](StorageFolder^ programFiles) { return programFiles->CreateFolderAsync("Win32Bridge", CreationCollisionOption::OpenIfExists); })
			.then([source](StorageFolder^ guestDirectory)
		{
			// A fixed guest name makes the virtual executable path deterministic.
			return create_task(source->CopyAsync(guestDirectory, "CustomGuest.exe", NameCollisionOption::ReplaceExisting));
		});
	})
		.then([this](StorageFile^ staged)
	{
		if (staged == nullptr)
			return task_from_result<IBuffer^>(nullptr);
		return create_task(FileIO::ReadBufferAsync(staged));
	})
		.then([this](IBuffer^ buffer)
	{
		if (buffer == nullptr)
			return;

		Platform::Array<byte>^ bytes = ref new Platform::Array<byte>(buffer->Length);
		DataReader::FromBuffer(buffer)->ReadBytes(bytes);
		PrepareGuest(bytes, L"C:\\Program Files\\Win32Bridge\\CustomGuest.exe", L"custom executable");
	})
		.then([this](task<void> previous)
	{
		try
		{
			previous.get();
		}
		catch (Platform::Exception^ error)
		{
			GuestStatusText->Text = "Could not stage the custom guest. Error: " + error->Message;
		}
		LoadGuestButton->IsEnabled = true;
	});
}

void MainPage::PrepareGuest(Platform::Array<byte>^ bytes, const std::wstring& guestPath, const std::wstring& sourceLabel)
{
	m_guestBytes = bytes;
	m_guestStorage = std::make_shared<Bridge::GuestStorageContext>(ApplicationData::Current->LocalFolder, guestPath);
	RunGuestButton->IsEnabled = false;

	Bridge::PeImageInfo image;
	if (!Bridge::PeImage::Inspect(bytes->Data, bytes->Length, &image))
	{
		GuestStatusText->Text = "Guest inspection failed.";
		ApiReportText->Text = ref new String(image.error.c_str());
		return;
	}

	Bridge::MappedPeImage mappedImage;
	std::wstring mappingError;
	if (!Bridge::PeMapper::Materialize(bytes->Data, bytes->Length, &mappedImage, &mappingError) ||
		!Bridge::PeMapper::ApplyBaseRelocations(&mappedImage, mappedImage.preferredImageBase, &mappingError))
	{
		GuestStatusText->Text = "Guest mapping failed.";
		ApiReportText->Text = ref new String(mappingError.c_str());
		return;
	}

	Bridge::BindingReport binding;
	std::wstring bindingError;
	if (!Bridge::ImportBinder::Bind(&mappedImage, image, Bridge::ResolveRuntimeImport, &binding, &bindingError))
	{
		GuestStatusText->Text = "Guest import binding failed.";
		ApiReportText->Text = ref new String(bindingError.c_str());
		return;
	}

	unsigned int adapters = 0;
	unsigned int deferred = 0;
	std::wstring report = L"PE32+ x64\nEntry point RVA (decimal): " + std::to_wstring(image.entryPointRva) + L"\n\n";
	for (size_t index = 0; index < image.imports.size(); ++index)
	{
		const auto& symbol = image.imports[index];
		const auto& resolution = binding.resolutions[index];
		if (resolution.disposition == Bridge::ImportDisposition::NeedsBridge) ++adapters;
		if (resolution.disposition == Bridge::ImportDisposition::Deferred) ++deferred;
		report += L"[";
		report += DispositionLabel(resolution.disposition);
		report += L"] " + symbol.library + L"!";
		report += symbol.importedByOrdinal ? L"#" + std::to_wstring(symbol.ordinal) : symbol.name;
		report += L"\n    " + resolution.note + L"\n";
	}

	const std::wstring summary = L"x64 PE accepted. " + std::to_wstring(image.imports.size()) +
		L" imports; IAT: " + std::to_wstring(binding.bound) + L" bound, " +
		std::to_wstring(binding.unresolved) + L" unresolved. Catalog: " +
		std::to_wstring(adapters) + L" adapter(s), " + std::to_wstring(deferred) + L" pending.";
	GuestStatusText->Text = ref new String((sourceLabel + L" staged in LocalFolder\\drive_c. " + summary).c_str());
	RunGuestButton->IsEnabled = binding.unresolved == 0;
	ApiReportText->Text = ref new String(report.c_str());
	SaveImportReport(L"Win32Bridge import report: " + sourceLabel + L"\n" + summary + L"\n\n" + report);
}

void MainPage::SaveImportReport(const std::wstring& report)
{
	const auto text = ref new String(report.c_str());
	create_task(ApplicationData::Current->LocalFolder->CreateFileAsync(
		"Win32Bridge-imports.log", CreationCollisionOption::ReplaceExisting))
		.then([text](StorageFile^ file)
	{
		return create_task(FileIO::WriteTextAsync(file, text));
	})
		.then([](task<void> completed)
	{
			try
			{
				completed.get();
			}
			catch (...)
			{
				// Diagnostics must never prevent the guest from being prepared.
			}
		});
}

void MainPage::RunGuest_Click(Object^, RoutedEventArgs^)
{
	if (!m_guestBytes || !m_guestStorage || !m_guestWindows)
	{
		GuestStatusText->Text = "Guest PE is not available.";
		return;
	}

	RunGuestButton->IsEnabled = false;
	if (m_runtimeLogTimer)
	{
		m_runtimeLogTimer->Cancel();
		m_runtimeLogTimer = nullptr;
	}
	Bridge::RuntimeDiagnostics::Reset(L"guest execution");
	Bridge::RuntimeDiagnostics::Record(L"HOST: Run guest requested.");
	SaveRuntimeReportSnapshot();
	TimeSpan runtimeLogPeriod = {};
	runtimeLogPeriod.Duration = 10000000; // One second in 100-nanosecond ticks.
	m_runtimeLogTimer = ThreadPoolTimer::CreatePeriodicTimer(
		ref new TimerElapsedHandler([](ThreadPoolTimer^)
	{
		SaveRuntimeReportSnapshot();
	}), runtimeLogPeriod);
	GuestSurfacePlaceholder->Visibility = ::Windows::UI::Xaml::Visibility::Collapsed;
	GuestStatusText->Text = "Preparing the guest runtime...";
	const auto guestBytes = m_guestBytes;
	const auto guestStorage = m_guestStorage;
	const auto guestWindows = m_guestWindows;
	const auto runtimeLogTimer = m_runtimeLogTimer;
	const auto dispatcher = Window::Current->Dispatcher;
	Platform::Agile<MainPage^> agilePage(this);
	ThreadPool::RunAsync(ref new WorkItemHandler([agilePage, guestBytes, guestStorage, guestWindows, dispatcher, runtimeLogTimer](IAsyncAction^)
	{
		Bridge::GuestRuntime runtime;
		runtime.SetStorageContext(guestStorage);
		runtime.SetWindowManager(guestWindows);
		std::wstring error;
		int exitCode = 0;
		const bool prepared = runtime.Prepare(guestBytes->Data, guestBytes->Length, Bridge::ResolveRuntimeImport, &error);
		SaveRuntimeReportSnapshot();
		if (prepared)
		{
			// GUI guests normally do not return until their own window is closed.
			// Report that transition before entering the guest's message loop, so
			// the host UI does not misleadingly remain at "Preparing" forever.
			dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([agilePage]()
			{
				MainPage^ page = agilePage.Get();
				if (page)
				{
					page->GuestStatusText->Text = "Guest runtime started; waiting for its first window...";
				}
			}));
		}
		const bool completed = prepared && runtime.Run(&exitCode, &error);
		if (runtimeLogTimer)
		{
			runtimeLogTimer->Cancel();
		}
		SaveRuntimeReportSnapshot();
		const std::wstring message = completed
			? L"Guest completed with exit code " + std::to_wstring(exitCode) + L"."
			: L"Guest runtime failed: " + error;
		const auto uiMessage = ref new String(message.c_str());

		dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([agilePage, uiMessage, completed]()
		{
			MainPage^ page = agilePage.Get();
			if (!page) return;
			page->GuestStatusText->Text = uiMessage;
			if (!completed) page->GuestSurfacePlaceholder->Visibility = ::Windows::UI::Xaml::Visibility::Visible;
			page->RunGuestButton->IsEnabled = true;
		}));
	}));
}
