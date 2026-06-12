<?php
/******************* Proxy for Babe32 browser  ***************

 * 190326 New from Claude Code Babe32 project
 * 120626 Browser UA, forward origin HTTP status, token auth
 * 120626 Brightdata residential proxy fallback for blocked sites
 * 120626 Full Chrome 120 header set; CURLOPT_ENCODING for auto-decompress
*/

  require_once __DIR__ . '/babe32proxy_secrets.php';
  $token = isset($_GET['token']) ? $_GET['token'] : '';
  if ($token !== $PROXY_TOKEN) { http_response_code(403); exit; }

  $url = isset($_GET['url']) ? $_GET['url'] : '';
  if (!$url) { http_response_code(400); exit; }

  function do_fetch($url, $brightdata_user = null, $brightdata_pass = null) {
      $ch = curl_init($url);
      $opts = [
          CURLOPT_RETURNTRANSFER => true,
          CURLOPT_FOLLOWLOCATION => true,
          CURLOPT_MAXREDIRS      => 5,
          CURLOPT_USERAGENT      => 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36',
          CURLOPT_ENCODING       => '',   // auto Accept-Encoding + transparent decompress
          CURLOPT_HTTPHEADER     => [
              'Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7',
              'Accept-Language: en-GB,en;q=0.9',
              'Upgrade-Insecure-Requests: 1',
              'Sec-Fetch-Dest: document',
              'Sec-Fetch-Mode: navigate',
              'Sec-Fetch-Site: none',
              'Sec-Fetch-User: ?1',
              'sec-ch-ua: "Not_A Brand";v="8", "Chromium";v="120", "Google Chrome";v="120"',
              'sec-ch-ua-mobile: ?0',
              'sec-ch-ua-platform: "Windows"',
              'Cache-Control: max-age=0',
          ],
          CURLOPT_TIMEOUT        => 15,
          CURLOPT_SSL_VERIFYPEER => false,
      ];
      if ($brightdata_user) {
          $opts[CURLOPT_PROXY]        = 'brd.superproxy.io:33335';
          $opts[CURLOPT_PROXYUSERPWD] = "$brightdata_user:$brightdata_pass";
      }
      curl_setopt_array($ch, $opts);
      $body   = curl_exec($ch);
      $status = curl_getinfo($ch, CURLINFO_HTTP_CODE);
      curl_close($ch);
      return [$status, $body];
  }

  [$status, $body] = do_fetch($url);

  // Retry via Brightdata residential proxy if blocked
  if ($status === 403 || $status === 429 || $status === 503 || $status === 0) {
      [$status, $body] = do_fetch($url, $BRIGHTDATA_USER, $BRIGHTDATA_PASS);
  }

  http_response_code($status ?: 502);
  echo $body;

?>
