using Windows.UI.Xaml.Controls;
using System;
using Windows.UI.Xaml.Navigation;

namespace factoryos_10x_shell.Views
{
    public sealed partial class FallbackErrorPage : Page
    {
        public FallbackErrorPage()
        {
            this.InitializeComponent();
        }

        protected override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);

            var exception = e.Parameter as Exception;
            if (exception == null)
            {
                ExceptionMessage.Text = "Tipo: desconhecido\nMensagem: erro sem detalhes.";
                StackTrace.Text = string.Empty;
                return;
            }

            ExceptionMessage.Text = $"Tipo: {exception.GetType().FullName}\nMensagem: {exception.Message}";
            StackTrace.Text = $"StackTrace:\n{exception}";
        }
    }
}
