/* iOS host for dsp56kBench.
 *
 * A bundle whose main() only computes is SIGKILLed once it stops answering the
 * system -- about 20 seconds -- which is not enough to boot a synth's firmware,
 * let alone render. Becoming a real UIApplication and running the benchmark on
 * a background queue removes that ceiling: the main thread stays responsive on
 * its run loop while the emulation takes as long as it needs.
 */
#import <UIKit/UIKit.h>

extern "C" int runDsp56kBench(void);

@interface BenchAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow* window;
@end

@implementation BenchAppDelegate

- (BOOL)application:(UIApplication*)application
	didFinishLaunchingWithOptions:(NSDictionary*)launchOptions
{
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	UIViewController* vc = [UIViewController new];
	vc.view.backgroundColor = UIColor.blackColor;
	self.window.rootViewController = vc;
	[self.window makeKeyAndVisible];

	[UIApplication.sharedApplication setIdleTimerDisabled:YES];

	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		runDsp56kBench();
		NSLog(@"dsp56kBench: run complete");
	});

	return YES;
}

@end

int main(int argc, char* argv[])
{
	@autoreleasepool
	{
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([BenchAppDelegate class]));
	}
}
