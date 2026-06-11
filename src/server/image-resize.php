<?php
/******************* Image resize proxy for Babe32 browser ***************

* 100626 New — fetches image from URL, resizes to fit ip_width x ip_height,
*         returns JPEG at ip_quality. If source fits already, returns as-is.
*/

$url     = isset($_GET['url'])      ? $_GET['url']            : '';
$width   = isset($_GET['ip_width']) ? (int)$_GET['ip_width']  : 480;
$height  = isset($_GET['ip_height'])? (int)$_GET['ip_height'] : 310;
$quality = isset($_GET['ip_quality'])?(int)$_GET['ip_quality']: 80;

if (!$url) { http_response_code(400); exit; }

// Clamp to sane limits
$width   = max(1, min($width,  1920));
$height  = max(1, min($height, 1920));
$quality = max(1, min($quality,  100));

// Fetch the image
$ch = curl_init($url);
curl_setopt_array($ch, [
    CURLOPT_RETURNTRANSFER => true,
    CURLOPT_FOLLOWLOCATION => true,
    CURLOPT_MAXREDIRS      => 5,
    CURLOPT_USERAGENT      => 'Mozilla/5.0 (compatible)',
    CURLOPT_TIMEOUT        => 15,
    CURLOPT_SSL_VERIFYPEER => false,
]);
$data = curl_exec($ch);
$ctype = curl_getinfo($ch, CURLINFO_CONTENT_TYPE);
curl_close($ch);

if ($data === false || strlen($data) === 0) { http_response_code(502); exit; }

// Try to load as image
$src = @imagecreatefromstring($data);
if (!$src) {
    // Not a recognised image format — return raw bytes unchanged
    header('Content-Type: ' . ($ctype ?: 'application/octet-stream'));
    header('Content-Length: ' . strlen($data));
    echo $data;
    exit;
}

$src_w = imagesx($src);
$src_h = imagesy($src);

// Only scale down — never upscale
if ($src_w <= $width && $src_h <= $height) {
    // Source already fits; return as JPEG
    header('Content-Type: image/jpeg');
    ob_start();
    imagejpeg($src, null, $quality);
    $out = ob_get_clean();
    imagedestroy($src);
    header('Content-Length: ' . strlen($out));
    echo $out;
    exit;
}

// Scale to fit within width x height, preserving aspect ratio
$scale = min($width / $src_w, $height / $src_h);
$dst_w = (int)round($src_w * $scale);
$dst_h = (int)round($src_h * $scale);

$dst = imagecreatetruecolor($dst_w, $dst_h);
imagecopyresampled($dst, $src, 0, 0, 0, 0, $dst_w, $dst_h, $src_w, $src_h);
imagedestroy($src);

header('Content-Type: image/jpeg');
ob_start();
imagejpeg($dst, null, $quality);
$out = ob_get_clean();
imagedestroy($dst);

header('Content-Length: ' . strlen($out));
echo $out;

?>
