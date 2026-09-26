//
// MainPage.xaml.h
// Declaração da classe MainPage.
//

#pragma once

#include "MainPage.g.h"
#include "Bridge\\GuestStorage.h"
#include "Bridge\\GuestWindow.h"

#include <windows.system.threading.h>

#include <memory>
#include <string>

namespace Win32Bridge
{
	/// <summary>
	/// Uma página vazia que pode ser usada isoladamente ou navegada dentro de um Quadro.
	/// </summary>
	public ref class MainPage sealed
	{
	public:
		MainPage();

	private:
		void Test7Zip_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ args);
		void RunGuest_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ eventArgs);
		void LoadGuest_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ eventArgs);
		void Run7Zip_Click(Platform::Object^ sender, Windows::UI::Xaml::RoutedEventArgs^ eventArgs);
		void Stage7Zip();
		void PrepareGuest(Platform::Array<byte>^ bytes, const std::wstring& guestPath, const std::wstring& sourceLabel);
		void SaveImportReport(const std::wstring& report);
		Platform::Array<byte>^ m_guestBytes;
		Platform::Array<byte>^ m_sevenZipBytes;
		std::shared_ptr<Bridge::GuestStorageContext> m_guestStorage;
		std::shared_ptr<Bridge::GuestStorageContext> m_sevenZipStorage;
		std::shared_ptr<Bridge::GuestWindowManager> m_guestWindows;
		Windows::System::Threading::ThreadPoolTimer^ m_runtimeLogTimer;
	};
}
