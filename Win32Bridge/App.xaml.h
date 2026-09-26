//
// App.xaml.h
// Declaração da classe App.
//

#pragma once

#include "App.g.h"

namespace Win32Bridge
{
	/// <summary>
	///Fornece o comportamento específico do aplicativo para complementar a classe Application padrão.
	/// </summary>
	ref class App sealed
	{
	protected:
		virtual void OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^ e) override;

	internal:
		App();

	private:
		void OnSuspending(Platform::Object^ sender, Windows::ApplicationModel::SuspendingEventArgs^ e);
		void OnNavigationFailed(Platform::Object ^sender, Windows::UI::Xaml::Navigation::NavigationFailedEventArgs ^e);
	};
}
